/*
     File: SA_Device.cpp
 Abstract:  Part of SimpleAudioDriver Plug-In Example
  Version: 1.0.1

 Disclaimer: IMPORTANT:  This Apple software is supplied to you by Apple
 Inc. ("Apple") in consideration of your agreement to the following
 terms, and your use, installation, modification or redistribution of
 this Apple software constitutes acceptance of these terms.  If you do
 not agree with these terms, please do not use, install, modify or
 redistribute this Apple software.

 In consideration of your agreement to abide by the following terms, and
 subject to these terms, Apple grants you a personal, non-exclusive
 license, under Apple's copyrights in this original Apple software (the
 "Apple Software"), to use, reproduce, modify and redistribute the Apple
 Software, with or without modifications, in source and/or binary forms;
 provided that if you redistribute the Apple Software in its entirety and
 without modifications, you must retain this notice and the following
 text and disclaimers in all such redistributions of the Apple Software.
 Neither the name, trademarks, service marks or logos of Apple Inc. may
 be used to endorse or promote products derived from the Apple Software
 without specific prior written permission from Apple.  Except as
 expressly stated in this notice, no other rights or licenses, express or
 implied, are granted by Apple herein, including but not limited to any
 patent rights that may be infringed by your derivative works or by other
 works in which the Apple Software may be incorporated.

 The Apple Software is provided by Apple on an "AS IS" basis.  APPLE
 MAKES NO WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION
 THE IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS
 FOR A PARTICULAR PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND
 OPERATION ALONE OR IN COMBINATION WITH YOUR PRODUCTS.

 IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL
 OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION,
 MODIFICATION AND/OR DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED
 AND WHETHER UNDER THEORY OF CONTRACT, TORT (INCLUDING NEGLIGENCE),
 STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.

 Copyright (C) 2013 Apple Inc. All Rights Reserved.

*/
/*==================================================================================================
	SA_Device.cpp
==================================================================================================*/

//==================================================================================================
//	Includes
//==================================================================================================

//	Self Include
#include "SA_Device.h"

//	Local Includes
#include "SA_PlugIn.h"

//	PublicUtility Includes
#include "CADebugMacros.h"
#include "CADispatchQueue.h"
#include "CAException.h"

#include <mach/mach_time.h>
#include <cstdlib>

//==================================================================================================
//	SA_Device
//==================================================================================================
#pragma mark Construction/Destruction

SA_Device::SA_Device(AudioObjectID inObjectID, UInt32 instance)
:
	SA_Object(inObjectID, kAudioDeviceClassID, kAudioObjectClassID, kAudioObjectPlugInObject),
    JackBridgeDriverIF(instance),
	mStateMutex("Device State"),
	mIOMutex("Device IO"),
	mStartCount(0),
	mSampleRateShadow(48000),
	mRingBufferFrameSize(0),
	mDriverStatus(JB_DRV_STATUS_INIT),
	mDeviceIsAlive(true),
	mLastDaemonAlive(0),
	mLastDaemonAliveHostTime(0),
	mHealthCycleCount(0),
	mHealthLastHostTime(0),
	mHealthMaxNFrames(0),
	mHealthNearMiss(0),
	mHealthLeadJitter(0),
	mSafetyOffsetFrames(0)
{
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
	    mInputStreamObjectID[i] = SA_ObjectMap::GetNextObjectID();
	    mInputStreamIsActive[i] = true;
    }

	for(int i=0; i<kNumberOfOutputSubObjects; i++)
    {
	    mOutputStreamObjectID[i] = SA_ObjectMap::GetNextObjectID();
	    mOutputStreamIsActive[i] = true;
    }
}

void	SA_Device::Activate()
{
	//	Open the connection to the driver and initialize things.
	_HW_Open();

	//	map the subobject IDs to this object
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
	    SA_ObjectMap::MapObject(mInputStreamObjectID[i], this);
    }

	for(int i=0; i<kNumberOfOutputSubObjects; i++)
    {
	    SA_ObjectMap::MapObject(mOutputStreamObjectID[i], this);
    }

	//	call the super-class, which just marks the object as active
	SA_Object::Activate();

    //  calculate the host ticks per frame
    struct mach_timebase_info theTimeBaseInfo;
    mach_timebase_info(&theTimeBaseInfo);
    // Float64 cast is load-bearing: denom/numer are uint32_t. On Apple Silicon
    // numer=125, denom=3 → integer division yields 0, then ×1e9 stays 0, and
    // gDevice_HostTicksPerFrame ends up 0.0 - which silently disables every
    // host-time→frame conversion downstream (jitter measurement, etc.).
    Float64 theHostClockFrequency = (Float64)theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer;
    theHostClockFrequency *= 1000000000.0;
    gDevice_HostTicksPerFrame = theHostClockFrequency / mSampleRateShadow;
}

void	SA_Device::Deactivate()
{
	//	When this method is called, the obejct is basically dead, but we still need to be thread
	//	safe. In this case, we also need to be safe vs. any IO threads, so we need to take both
	//	locks.
	CAMutex::Locker theStateLocker(mStateMutex);
	CAMutex::Locker theIOLocker(mIOMutex);

	//	mark the object inactive by calling the super-class
	SA_Object::Deactivate();

	//	unmap the subobject IDs
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
	    SA_ObjectMap::UnmapObject(mInputStreamObjectID[i], this);
    }

	for(int i=0; i<kNumberOfOutputSubObjects; i++)
    {
	    SA_ObjectMap::UnmapObject(mOutputStreamObjectID[i], this);
    }

	//	close the connection to the driver
	_HW_Close();
}

SA_Device::~SA_Device()
{
}

bool	SA_Device::IsStreamObjectID(AudioObjectID inObjectID) const
{
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
        if (inObjectID == mInputStreamObjectID[i])
            return true;
    }

	for(int i=0; i<kNumberOfOutputSubObjects; i++)
    {
        if (inObjectID == mOutputStreamObjectID[i])
            return true;
    }
    return false;
}

bool 	SA_Device::IsInputStreamID(AudioObjectID inObjectID) const
{
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
        if (inObjectID == mInputStreamObjectID[i])
            return true;
    }
    return false;
}

int 	SA_Device::getStreamID(AudioObjectID inObjectID) const
{
	for(int i=0; i<kNumberOfInputSubObjects; i++)
    {
        if (inObjectID == mInputStreamObjectID[i])
            return i;
    }
	for(int i=0; i<kNumberOfOutputSubObjects; i++)
    {
        if (inObjectID == mOutputStreamObjectID[i])
            return i;
    }
    return 0;
}

#pragma mark Property Operations
bool	SA_Device::HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	This object implements several API-level objects. So the first thing to do is to figure out
	//	which object this request is really for. Note that mSubObjectID is an invariant as this
	//	driver's structure does not change dynamically. It will always have the parts it has.
	bool theAnswer = false;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_HasProperty(inObjectID, inClientPID, inAddress);
	}
	else if(IsStreamObjectID(inObjectID))
	{
		theAnswer = Stream_HasProperty(inObjectID, inClientPID, inAddress);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

bool	SA_Device::IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	bool theAnswer = false;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_IsPropertySettable(inObjectID, inClientPID, inAddress);
	}
	else if(IsStreamObjectID(inObjectID))
	{
		theAnswer = Stream_IsPropertySettable(inObjectID, inClientPID, inAddress);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

UInt32	SA_Device::GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	UInt32 theAnswer = 0;
	if(inObjectID == mObjectID)
	{
		theAnswer = Device_GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
	}
	else if(IsStreamObjectID(inObjectID))
	{
		theAnswer = Stream_GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
	return theAnswer;
}

void	SA_Device::GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
    //syslog(LOG_WARNING, "JackBridge: Call GetPropertyData %d. ", instance);
	if(inObjectID == mObjectID)
	{
		Device_GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	}
	else if(IsStreamObjectID(inObjectID))
	{
		Stream_GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
}

void	SA_Device::SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	if(inObjectID == mObjectID)
	{
		Device_SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
	}
	else if(IsStreamObjectID(inObjectID))
	{
		Stream_SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
	}
	else
	{
		Throw(CAException(kAudioHardwareBadObjectError));
	}
}

#pragma mark Device Property Operations

bool	SA_Device::Device_HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.

	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod:
		case kAudioDevicePropertyStreams:
			theAnswer = true;
			break;

		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			theAnswer = (inAddress.mScope == kAudioObjectPropertyScopeInput) || (inAddress.mScope == kAudioObjectPropertyScopeOutput);
			break;

		case kAudioObjectPropertyElementName:
			//	DAWs (REAPER in particular) query channel labels at Device scope
			//	with mElement = absolute 1-based channel index, not via Stream
			//	objects. Mirror the Stream-level handler so the names show up
			//	regardless of which scope the host walks.
			theAnswer = ((inAddress.mScope == kAudioObjectPropertyScopeInput  && inAddress.mElement >= 1 && inAddress.mElement <= 4) ||
			             (inAddress.mScope == kAudioObjectPropertyScopeOutput && inAddress.mElement >= 1 && inAddress.mElement <= 2));
			break;

		default:
			theAnswer = SA_Object::HasProperty(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

bool	SA_Device::Device_IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.

	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertyStreams:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			theAnswer = false;
			break;

		case kAudioDevicePropertyNominalSampleRate:
			theAnswer = true;
			break;

		default:
			theAnswer = SA_Object::IsPropertySettable(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

UInt32	SA_Device::Device_GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.

	UInt32 theAnswer = 0;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyName:
			theAnswer = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyManufacturer:
			theAnswer = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					theAnswer = kNumberOfSubObjects * sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyScopeInput:
					theAnswer = kNumberOfInputSubObjects * sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyScopeOutput:
					theAnswer = kNumberOfOutputSubObjects * sizeof(AudioObjectID);
					break;
			};
			break;

		case kAudioDevicePropertyDeviceUID:
			theAnswer = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			theAnswer = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			theAnswer = sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			theAnswer = sizeof(AudioClassID);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					theAnswer = kNumberOfStreams * sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyScopeInput:
					theAnswer = kNumberOfInputStreams * sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyScopeOutput:
					theAnswer = kNumberOfOutputStreams * sizeof(AudioObjectID);
					break;
			};
			break;

		case kAudioObjectPropertyControlList:
			theAnswer = kNumberOfControls * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertySafetyOffset:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			theAnswer = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			theAnswer = 2 * sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyIsHidden:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			theAnswer = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			theAnswer = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioObjectPropertyElementName:
			theAnswer = sizeof(CFStringRef);
			break;

		default:
			theAnswer = SA_Object::GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
			break;
	};
	return theAnswer;
}

void	SA_Device::Device_GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required.
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.

	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyName:
			//	This is the human readable name of the device. Note that in this case we return a
			//	value that is a key into the localizable strings in this bundle. This allows us to
			//	return a localized name for the device.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
            *reinterpret_cast<CFStringRef*>(outData) = CFSTR("DeviceName");
			outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in. Note that in this case
			//	we return a value that is a key into the localizable strings in this bundle. This
			//	allows us to return a localized name for the manufacturer.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR("ManufacturerName");
			outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	The device owns its streams and controls. Note that what is returned here
			//	depends on the scope requested.
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all objects
					if(theNumberItemsToFetch > kNumberOfSubObjects)
					{
						theNumberItemsToFetch = kNumberOfSubObjects;
					}

					//	fill out the list with as many objects as requested, which is everything
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfInputSubObjects)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mInputStreamObjectID[i];
					    }
                        else if(i < kNumberOfSubObjects)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mOutputStreamObjectID[i-kNumberOfInputSubObjects];
                        }
                    }
					break;

				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > kNumberOfInputSubObjects)
					{
						theNumberItemsToFetch = kNumberOfInputSubObjects;
					}

					//	fill out the list with the right objects
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfInputSubObjects)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mInputStreamObjectID[i];
					    }
                    }
					break;

				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > kNumberOfOutputSubObjects)
					{
						theNumberItemsToFetch = kNumberOfOutputSubObjects;
					}

					//	fill out the list with the right objects
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfOutputSubObjects)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mOutputStreamObjectID[i];
					    }
                    }
					break;
			};

			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyDeviceUID:
			//	This is a CFString that is a persistent token that can identify the same
			//	audio device across boot sessions. Note that two instances of the same
			//	device must have different values for this property.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kDeviceUID);
			outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			//	This is a CFString that is a persistent token that can identify audio
			//	devices that are the same kind of device. Note that two instances of the
			//	save device must have the same value for this property.
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
			*reinterpret_cast<CFStringRef*>(outData) = CFSTR(kDeviceModelUID);
			outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			//	This value represents how the device is attached to the system. This can be
			//	any 32 bit integer, but common values for this property are defined in
			//	<CoreAudio/AudioHardwareBase.h>
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the device");
			*reinterpret_cast<UInt32*>(outData) = kAudioDeviceTransportTypeVirtual;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			//	The related devices property identifies device objects that are very closely
			//	related. Generally, this is for relating devices that are packaged together
			//	in the hardware such as when the input side and the output side of a piece
			//	of hardware can be clocked separately and therefore need to be represented
			//	as separate AudioDevice objects. In such case, both devices would report
			//	that they are related to each other. Note that at minimum, a device is
			//	related to itself, so this list will always be at least one item long.

			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	we only have the one device...
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}

			//	Write the devices' object IDs into the return value
			if(theNumberItemsToFetch > 0)
			{
				reinterpret_cast<AudioObjectID*>(outData)[0] = GetObjectID();
			}

			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			//	This property allows the device to declare what other devices it is
			//	synchronized with in hardware. The way it works is that if two devices have
			//	the same value for this property and the value is not zero, then the two
			//	devices are synchronized in hardware. Note that a device that either can't
			//	be synchronized with others or doesn't know should return 0 for this
			//	property.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyClockDomain for the device");
			*reinterpret_cast<UInt32*>(outData) = 0;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			//	Reflects the daemon-heartbeat watchdog in GetZeroTimeStamp -
			//	flips to 0 when jackd dies so the DAW disconnects cleanly
			//	instead of getting forever-silence.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsAlive for the device");
			*reinterpret_cast<UInt32*>(outData) = mDeviceIsAlive.load(std::memory_order_acquire) ? 1 : 0;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			//	This property returns whether or not IO is running for the device.
			{
				ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");

				//	The IsRunning state is protected by the state lock
				CAMutex::Locker theStateLocker(mStateMutex);

				//	return the state and how much data we are touching
				*reinterpret_cast<UInt32*>(outData) = mStartCount > 0;
				outDataSize = sizeof(UInt32);
			}
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			//	This property returns whether or not the device wants to be able to be the
			//	default device for content. This is the device that iTunes and QuickTime
			//	will use to play their content on and FaceTime will use as it's microhphone.
			//	Nearly all devices should allow for this.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultDevice for the device");
			*reinterpret_cast<UInt32*>(outData) = 1;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			//	This property returns whether or not the device wants to be the system
			//	default device. This is the device that is used to play interface sounds and
			//	other incidental or UI-related sounds on. Most devices should allow this
			//	although devices with lots of latency may not want to.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultSystemDevice for the device");
			*reinterpret_cast<UInt32*>(outData) = 1;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			//	Presentation latency of the device. We report the end-to-end chain
			//	documented in docs/LATENCY-MODEL.md *excluding* JitterFrames, which
			//	is surfaced separately via kAudioDevicePropertySafetyOffset. The DAW
			//	sums Latency + SafetyOffset + BufferFrameSize + StreamLatency.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device");
			if (inAddress.mScope == kAudioObjectPropertyScopeInput) {
				*reinterpret_cast<UInt32*>(outData) = mReportedLatencyInput;
			} else if (inAddress.mScope == kAudioObjectPropertyScopeOutput) {
				*reinterpret_cast<UInt32*>(outData) = mReportedLatencyOutput;
			} else {
				*reinterpret_cast<UInt32*>(outData) = 0;
			}
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	Note that what is returned here depends on the scope requested.
			switch(inAddress.mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all streams
					if(theNumberItemsToFetch > kNumberOfStreams)
					{
						theNumberItemsToFetch = kNumberOfStreams;
					}

					//	fill out the list with as many objects as requested
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfInputStreams)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mInputStreamObjectID[i];
					    }
                        else if(i < kNumberOfStreams)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mOutputStreamObjectID[i-kNumberOfInputStreams];
                        }
                    }
					break;

				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > kNumberOfInputStreams)
					{
						theNumberItemsToFetch = kNumberOfInputStreams;
					}

					//	fill out the list with as many objects as requested
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfInputStreams)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mInputStreamObjectID[i];
					    }
                    }
					break;

				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > kNumberOfOutputStreams)
					{
						theNumberItemsToFetch = kNumberOfOutputStreams;
					}

					//	fill out the list with as many objects as requested
                    for (UInt32 i=0; i<theNumberItemsToFetch; i++)
                    {
					    if(i < kNumberOfOutputStreams)
                        {
						    reinterpret_cast<AudioObjectID*>(outData)[i] = mOutputStreamObjectID[i];
					    }
                    }
					break;
			};

			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyControlList:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			outDataSize = 0;
			break;

		case kAudioDevicePropertySafetyOffset:
			//	Producer-side safety lead. CoreAudio schedules the IOProc this many
			//	frames earlier in sampleTime, giving the daemon time to land each
			//	period in the ring before the HAL reads it. Set from config.plist
			//	JitterFrames in _HW_Open.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device");
			*reinterpret_cast<UInt32*>(outData) = mSafetyOffsetFrames;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			//	This property returns the nominal sample rate of the device. Note that we
			//	only need to take the state lock to get this value.
			{
				ThrowIf(inDataSize < sizeof(Float64), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");

				//	The sample rate is protected by the state lock
				CAMutex::Locker theStateLocker(mStateMutex);

				//	need to lock around fetching the sample rate
				*reinterpret_cast<Float64*>(outData) = static_cast<Float64>(_HW_GetSampleRate());
				outDataSize = sizeof(Float64);
			}
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			//	This returns all nominal sample rates the device supports as an array of
			//	AudioValueRangeStructs. Note that for discrete sampler rates, the range
			//	will have the minimum value equal to the maximum value.

			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);

			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > 2)
			{
				theNumberItemsToFetch = 2;
			}

			//	fill out the return array
			if(theNumberItemsToFetch > 0)
			{
				((AudioValueRange*)outData)[0].mMinimum = 44100.0;
				((AudioValueRange*)outData)[0].mMaximum = 44100.0;
			}
			if(theNumberItemsToFetch > 1)
			{
				((AudioValueRange*)outData)[1].mMinimum = 48000.0;
				((AudioValueRange*)outData)[1].mMaximum = 48000.0;
			}

			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyIsHidden:
			//	This returns whether or not the device is visible to clients.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyIsHidden for the device");
			*reinterpret_cast<UInt32*>(outData) = 0;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			//	This property returns which two channesl to use as left/right for stereo
			//	data by default. Note that the channel numbers are 1-based.
			ThrowIf(inDataSize < (2 * sizeof(UInt32)), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device");
			((UInt32*)outData)[0] = 1;
			((UInt32*)outData)[1] = 2;
			outDataSize = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			//	This property returns the default AudioChannelLayout to use for the device
			//	by default. For this device, we return a stereo ACL.
			{
				//	calcualte how big the
				UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
				ThrowIf(inDataSize < theACLSize, CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");
				((AudioChannelLayout*)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
				((AudioChannelLayout*)outData)->mChannelBitmap = 0;
				((AudioChannelLayout*)outData)->mNumberChannelDescriptions = 2;
				for(theItemIndex = 0; theItemIndex < 2; ++theItemIndex)
				{
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelLabel = kAudioChannelLabel_Left + theItemIndex;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelFlags = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0;
					((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0;
				}
				outDataSize = theACLSize;
			}
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			//	This property returns how many frames the HAL should expect to see between
			//	successive sample times in the zero time stamps this device provides.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
			*reinterpret_cast<UInt32*>(outData) = mRingBufferFrameSize;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioObjectPropertyElementName:
			//	Device-scope channel labels. REAPER (and others) query here
			//	with mElement = absolute 1-based channel index, not via Stream
			//	objects. Layout: 4 inputs [In1, In2, ModOut1, ModOut2],
			//	2 outputs [Out1, Out2]. The Stream-level handler stays for
			//	hosts that walk that path; this one covers Device-scope walkers.
			{
				ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyElementName for the device");
				CFStringRef name = CFSTR("");
				if (inAddress.mScope == kAudioObjectPropertyScopeInput) {
					switch (inAddress.mElement) {
						case 1: name = CFSTR("In1"); break;
						case 2: name = CFSTR("In2"); break;
						case 3: name = CFSTR("ModOut1"); break;
						case 4: name = CFSTR("ModOut2"); break;
					}
				} else if (inAddress.mScope == kAudioObjectPropertyScopeOutput) {
					switch (inAddress.mElement) {
						case 1: name = CFSTR("Out1"); break;
						case 2: name = CFSTR("Out2"); break;
					}
				}
				*reinterpret_cast<CFStringRef*>(outData) = (CFStringRef)CFRetain(name);
				outDataSize = sizeof(CFStringRef);
			}
			break;

		default:
			SA_Object::GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
	};
}

void	SA_Device::Device_SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Device_GetPropertyData() method.

	switch(inAddress.mSelector)
	{
		case kAudioDevicePropertyNominalSampleRate:
			//	Changing the sample rate needs to be handled via the RequestConfigChange/PerformConfigChange machinery.
			{
				//	check the arguments
				ThrowIf(inDataSize != sizeof(Float64), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Device_SetPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
				ThrowIf((*((const Float64*)inData) != 44100.0) && (*((const Float64*)inData) != 48000.0), CAException(kAudioHardwareIllegalOperationError), "SA_Device::Device_SetPropertyData: unsupported value for kAudioDevicePropertyNominalSampleRate");

				//	we need to lock around getting the current sample rate to compare against the new rate
				UInt64 theOldSampleRate = 0;
				{
					CAMutex::Locker theStateLocker(mStateMutex);
					theOldSampleRate = _HW_GetSampleRate();
				}

				//	make sure that the new value is different than the old value
				UInt64 theNewSampleRate = static_cast<UInt64>(*reinterpret_cast<const Float64*>(inData));
				if(theNewSampleRate != theOldSampleRate)
				{
					//	we dispatch this so that the change can happen asynchronously
					AudioObjectID theDeviceObjectID = GetObjectID();
					CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
																				SA_PlugIn::Host_RequestDeviceConfigurationChange(theDeviceObjectID, theNewSampleRate, NULL);
																			});
				}
			}
			break;

		default:
			SA_Object::SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
			break;
	};
}

#pragma mark Stream Property Operations

bool	SA_Device::Stream_HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Stream_GetPropertyData() method.

	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
		case kAudioObjectPropertyElementName:
			theAnswer = true;
			break;

		default:
			theAnswer = SA_Object::HasProperty(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

bool	SA_Device::Stream_IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Stream_GetPropertyData() method.

	bool theAnswer = false;
	switch(inAddress.mSelector)
	{
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			theAnswer = false;
			break;

		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			theAnswer = true;
			break;

		default:
			theAnswer = SA_Object::IsPropertySettable(inObjectID, inClientPID, inAddress);
			break;
	};
	return theAnswer;
}

UInt32	SA_Device::Stream_GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Stream_GetPropertyData() method.

	UInt32 theAnswer = 0;
	switch(inAddress.mSelector)
	{
		case kAudioStreamPropertyIsActive:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioStreamPropertyDirection:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioStreamPropertyLatency:
			theAnswer = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			theAnswer = sizeof(AudioStreamBasicDescription);
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			theAnswer = 2 * sizeof(AudioStreamRangedDescription);
			break;

		case kAudioObjectPropertyElementName:
			theAnswer = sizeof(CFStringRef);
			break;

		default:
			theAnswer = SA_Object::GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
			break;
	};
	return theAnswer;
}

void	SA_Device::Stream_GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required.
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.

	UInt32 theNumberItemsToFetch;
	switch(inAddress.mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioStreamClassID is kAudioObjectClassID
			ThrowIf(inDataSize < sizeof(AudioClassID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the volume control");
			*reinterpret_cast<AudioClassID*>(outData) = kAudioObjectClassID;
			outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			//	Streams are of the class, kAudioStreamClassID
			ThrowIf(inDataSize < sizeof(AudioClassID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the volume control");
			*reinterpret_cast<AudioClassID*>(outData) = kAudioStreamClassID;
			outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			//	The stream's owner is the device object
			ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the volume control");
			*reinterpret_cast<AudioObjectID*>(outData) = GetObjectID();
			outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioStreamPropertyIsActive:
			//	This property tells the device whether or not the given stream is going to
			//	be used for IO. Note that we need to take the state lock to examine this
			//	value.
			{
				ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyIsActive for the stream");

				//	lock the state mutex
				CAMutex::Locker theStateLocker(mStateMutex);

				//	return the requested value
				*reinterpret_cast<UInt32*>(outData) = (inAddress.mScope == kAudioObjectPropertyScopeInput) ? mInputStreamIsActive[getStreamID(inObjectID)] : mOutputStreamIsActive[getStreamID(inObjectID)];
				//*reinterpret_cast<UInt32*>(outData) = (inAddress.mScope == kAudioObjectPropertyScopeInput) ? mInputStreamIsActive : mOutputStreamIsActive;
				outDataSize = sizeof(UInt32);
			}
			break;

		case kAudioStreamPropertyDirection:
			//	This returns whether the stream is an input stream or an output stream.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyDirection for the stream");
			*reinterpret_cast<UInt32*>(outData) = IsInputStreamID(inObjectID) ? 1 : 0;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			//	This returns a value that indicates what is at the other end of the stream
			//	such as a speaker or headphones, or a microphone. Values for this property
			//	are defined in <CoreAudio/AudioHardwareBase.h>
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyTerminalType for the stream");
			*reinterpret_cast<UInt32*>(outData) = IsInputStreamID(inObjectID) ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			//	This property returns the absolute channel number for the first channel in
			//	the stream. For exmaple, if a device has two output streams with two
			//	channels each, then the starting channel number for the first stream is 1
			//	and ths starting channel number fo the second stream is 3.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			*reinterpret_cast<UInt32*>(outData) = getStreamID(inObjectID)*2+1;
			//*reinterpret_cast<UInt32*>(outData) = 1;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyLatency:
			//	This property returns any additonal presentation latency the stream has.
			ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			*reinterpret_cast<UInt32*>(outData) = 0;
			outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			//	This returns the current format of the stream in an AudioStreamBasicDescription.
			//	For devices that don't override the mix operation, the virtual format has to be the
			//	same as the physical format.
			{
				ThrowIf(inDataSize < sizeof(AudioStreamBasicDescription), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioStreamPropertyVirtualFormat for the stream");

				//	lock the state mutex
				CAMutex::Locker theStateLocker(mStateMutex);

				//	This particular device always vends  16 bit native endian signed integers
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mSampleRate = static_cast<Float64>(_HW_GetSampleRate());
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mFormatID = kAudioFormatLinearPCM;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mBytesPerPacket = 8;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mFramesPerPacket = 1;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mBytesPerFrame = 8;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mChannelsPerFrame = 2;
				reinterpret_cast<AudioStreamBasicDescription*>(outData)->mBitsPerChannel = 32;
				outDataSize = sizeof(AudioStreamBasicDescription);
			}
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			//	This returns an array of AudioStreamRangedDescriptions that describe what
			//	formats are supported.

			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioStreamRangedDescription);

			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > 2)
			{
				theNumberItemsToFetch = 2;
			}

			//	fill out the return array
			if(theNumberItemsToFetch > 0)
			{
				((AudioStreamRangedDescription*)outData)[0].mFormat.mSampleRate = 44100.0;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatID = kAudioFormatLinearPCM;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerPacket = 8;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFramesPerPacket = 1;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerFrame = 8;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mChannelsPerFrame = 2;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBitsPerChannel = 32;
				((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMinimum = 44100.0;
				((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMaximum = 44100.0;
			}
			if(theNumberItemsToFetch > 1)
			{
				((AudioStreamRangedDescription*)outData)[1].mFormat.mSampleRate = 48000.0;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mFormatID = kAudioFormatLinearPCM;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mBytesPerPacket = 8;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mFramesPerPacket = 1;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mBytesPerFrame = 8;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mChannelsPerFrame = 2;
				((AudioStreamRangedDescription*)outData)[1].mFormat.mBitsPerChannel = 32;
				((AudioStreamRangedDescription*)outData)[1].mSampleRateRange.mMinimum = 48000.0;
				((AudioStreamRangedDescription*)outData)[1].mSampleRateRange.mMaximum = 48000.0;
			}

			//	report how much we wrote
			outDataSize = theNumberItemsToFetch * sizeof(AudioStreamRangedDescription);
			break;

		case kAudioObjectPropertyElementName:
			//	Per-channel labels surfaced to DAWs. mElement is 1-based within
			//	the stream (1 = first channel, 2 = second). Layout: 4 inputs
			//	[In1, In2, ModOut1, ModOut2] across 2 stereo input streams,
			//	2 outputs [Out1, Out2] in 1 stereo output stream.
			{
				ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_GetPropertyData: not enough space for the return value of kAudioObjectPropertyElementName for the stream");
				int streamIdx = getStreamID(inObjectID);
				bool isInput = IsInputStreamID(inObjectID);
				UInt32 ch = inAddress.mElement;
				CFStringRef name = CFSTR("");
				if (isInput) {
					if (streamIdx == 0 && ch == 1) name = CFSTR("In1");
					else if (streamIdx == 0 && ch == 2) name = CFSTR("In2");
					else if (streamIdx == 1 && ch == 1) name = CFSTR("ModOut1");
					else if (streamIdx == 1 && ch == 2) name = CFSTR("ModOut2");
				} else {
					if (streamIdx == 0 && ch == 1) name = CFSTR("Out1");
					else if (streamIdx == 0 && ch == 2) name = CFSTR("Out2");
				}
				*reinterpret_cast<CFStringRef*>(outData) = (CFStringRef)CFRetain(name);
				outDataSize = sizeof(CFStringRef);
			}
			break;

		default:
			SA_Object::GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
	};
}

void	SA_Device::Stream_SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	//	For each object, this driver implements all the required properties plus a few extras that
	//	are useful but not required. There is more detailed commentary about each property in the
	//	Stream_GetPropertyData() method.

	switch(inAddress.mSelector)
	{
		case kAudioStreamPropertyIsActive:
			{
				//	Changing the active state of a stream doesn't affect IO or change the structure
				//	so we can just save the state and send the notification.
				ThrowIf(inDataSize != sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_SetPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
				bool theNewIsActive = *reinterpret_cast<const UInt32*>(inData) != 0;

				CAMutex::Locker theStateLocker(mStateMutex);
                for(int i=0; i<kNumberOfInputStreams; i++)
                {
				    if(inObjectID == mInputStreamObjectID[i])
                    {
                        if(mInputStreamIsActive[i] != theNewIsActive)
                        {
                            mInputStreamIsActive[i] = theNewIsActive;
                            break;
                        }
                    }
                }

                for(int i=0; i<kNumberOfOutputStreams; i++)
                {
				    if(inObjectID == mOutputStreamObjectID[i])
                    {
                        if(mOutputStreamIsActive[i] != theNewIsActive)
                        {
                            mOutputStreamIsActive[i] = theNewIsActive;
                            break;
                        }
                    }
                }
			}
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			{
				//	Changing the stream format needs to be handled via the
				//	RequestConfigChange/PerformConfigChange machinery. Note that because this
				//	device only supports 2 channel 32 bit float data, the only thing that can
				//	change is the sample rate.
				ThrowIf(inDataSize != sizeof(AudioStreamBasicDescription), CAException(kAudioHardwareBadPropertySizeError), "SA_Device::Stream_SetPropertyData: wrong size for the data for kAudioStreamPropertyPhysicalFormat");

				const AudioStreamBasicDescription* theNewFormat = reinterpret_cast<const AudioStreamBasicDescription*>(inData);
				ThrowIf(theNewFormat->mFormatID != kAudioFormatLinearPCM, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported format ID for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mFormatFlags != (kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked), CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported format flags for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mBytesPerPacket != 8, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported bytes per packet for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mFramesPerPacket != 1, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported frames per packet for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mBytesPerFrame != 8, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported bytes per frame for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mChannelsPerFrame != 2, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported channels per frame for kAudioStreamPropertyPhysicalFormat");
				ThrowIf(theNewFormat->mBitsPerChannel != 32, CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported bits per channel for kAudioStreamPropertyPhysicalFormat");
				ThrowIf((theNewFormat->mSampleRate != 44100.0) && (theNewFormat->mSampleRate != 48000.0), CAException(kAudioDeviceUnsupportedFormatError), "SA_Device::Stream_SetPropertyData: unsupported sample rate for kAudioStreamPropertyPhysicalFormat");

				//	we need to lock around getting the current sample rate to compare against the new rate
				UInt64 theOldSampleRate = 0;
				{
					CAMutex::Locker theStateLocker(mStateMutex);
					theOldSampleRate = _HW_GetSampleRate();
				}

				//	make sure that the new value is different than the old value
				UInt64 theNewSampleRate = static_cast<UInt64>(*reinterpret_cast<const Float64*>(inData));
				if(theNewSampleRate != theOldSampleRate)
				{
					//	we dispatch this so that the change can happen asynchronously
					AudioObjectID theDeviceObjectID = GetObjectID();
					CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
																				SA_PlugIn::Host_RequestDeviceConfigurationChange(theDeviceObjectID, theNewSampleRate, NULL);
																			});
				}
			}
			break;

		default:
			SA_Object::SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
			break;
	};
}

#pragma mark IO Operations

void	SA_Device::StartIO()
{
	//	Starting/Stopping IO needs to be reference counted due to the possibility of multiple clients starting IO
	CAMutex::Locker theStateLocker(mStateMutex);

	//	make sure we can start
	ThrowIf(mStartCount == UINT64_MAX, CAException(kAudioHardwareIllegalOperationError), "SA_Device::StartIO: failed to start because the ref count was maxxed out already");

	//	we only tell the hardware to start if this is the first time IO has been started
	if(mStartCount == 0)
	{
        gDevice_NumberTimeStamps = 0;
        gDevice_AnchorSampleTime = 0.0;

		kern_return_t theError = _HW_StartIO();
		ThrowIfKernelError(theError, CAException(theError), "SA_Device::StartIO: failed to start because of an error calling down to the driver");
	}
	++mStartCount;
}

void	SA_Device::StopIO()
{
	//	Starting/Stopping IO needs to be reference counted due to the possibility of multiple clients starting IO
	CAMutex::Locker theStateLocker(mStateMutex);

	//	we tell the hardware to stop if this is the last stop call
	if(mStartCount == 1)
	{
		_HW_StopIO();
		mStartCount = 0;
	}
	else if(mStartCount > 1)
	{
		--mStartCount;
	}
}

void	SA_Device::GetZeroTimeStamp(Float64& outSampleTime, UInt64& outHostTime, UInt64& outSeed)
{
    Float64 theHostTickOffset;
    UInt64 theNextHostTime;

    //  calculate the next host time
    Float64 theHostTicksPerRingBuffer = gDevice_HostTicksPerFrame * ((Float64)mRingBufferFrameSize);

    // Daemon liveness - compare the heartbeat counter against the previous
    // sample. If it hasn't advanced within ~5 cycles of host time, declare the
    // device dead so the DAW disconnects instead of getting forever-silence.
    // Threshold is in host-time units, not call counts, to stay robust if the
    // IO thread's call rate drifts.
    uint64_t now = mach_absolute_time();
    uint64_t curAlive = shmDaemonAlive->load(std::memory_order_acquire);
    if (curAlive != mLastDaemonAlive) {
        mLastDaemonAlive = curAlive;
        mLastDaemonAliveHostTime = now;
        if (!mDeviceIsAlive.load(std::memory_order_acquire)) {
            mDeviceIsAlive.store(true, std::memory_order_release);
            JB_LOG_INFO(jb_log_driver(),
                "daemon heartbeat resumed - flipping DeviceIsAlive=1");
            AudioObjectPropertyAddress addr = {
                kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            SA_PlugIn::Host_PropertiesChanged(GetObjectID(), 1, &addr);
        }
    } else if (mDeviceIsAlive.load(std::memory_order_acquire) &&
               mRingBufferFrameSize > 0 &&
               mLastDaemonAliveHostTime != 0 &&
               theHostTicksPerRingBuffer > 0.0) {
        // Guards above: skip the check before _HW_Open has set the ring-buffer
        // size, before we've ever observed a heartbeat tick (otherwise
        // `now - 0` is enormous on the first call), and before
        // gDevice_HostTicksPerFrame has been initialized (otherwise threshold
        // is 0 and `now - last > 0` is trivially true → log spam every IO
        // cycle).
        //
        // TODO(lifecycle): the gDevice_HostTicksPerFrame == 0 case is itself a
        // bug - that global is supposed to be set during _HW_Open / device
        // construction. Something is calling GetZeroTimeStamp before init
        // completes, or the global isn't being set on the path we think it is.
        // Find out where and fix the ordering instead of guarding here.
        UInt64 threshold = (UInt64)(5.0 * theHostTicksPerRingBuffer);
        if (now - mLastDaemonAliveHostTime > threshold) {
            mDeviceIsAlive.store(false, std::memory_order_release);
            JB_LOG_ERR(jb_log_driver(),
                "daemon heartbeat stalled >%llu host ticks - flipping DeviceIsAlive=0",
                (unsigned long long)threshold);
            AudioObjectPropertyAddress addr = {
                kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            SA_PlugIn::Host_PropertiesChanged(GetObjectID(), 1, &addr);
        }
    }
    theHostTickOffset = ((Float64)(gDevice_NumberTimeStamps + 1)) * theHostTicksPerRingBuffer;
    theNextHostTime = gDevice_AnchorHostTime + ((UInt64)theHostTickOffset);
    //  go to the next time if the next host time is less than the current time
    if(theNextHostTime <= mach_absolute_time())
    {
        ++gDevice_NumberTimeStamps;
    }

    //  set the return values
    if (shmSyncMode->load(std::memory_order_acquire) == 1) {
        outSampleTime = shmNumberTimeStamps->load(std::memory_order_acquire) * mRingBufferFrameSize;
        outHostTime = shmZeroHostTime->load(std::memory_order_acquire);
    } else {
        outSampleTime = gDevice_NumberTimeStamps * mRingBufferFrameSize;
        outHostTime = gDevice_AnchorHostTime + (((Float64)gDevice_NumberTimeStamps) * theHostTicksPerRingBuffer);
        shmNumberTimeStamps->store(gDevice_NumberTimeStamps, std::memory_order_relaxed);
        shmZeroHostTime->store(outHostTime, std::memory_order_release);
    }
    outSeed = shmSeed->load(std::memory_order_acquire);
}

void	SA_Device::WillDoIOOperation(UInt32 inOperationID, bool& outWillDo, bool& outWillDoInPlace) const
{
	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
		case kAudioServerPlugInIOOperationWriteMix:
			outWillDo = true;
			outWillDoInPlace = true;
			break;

		case kAudioServerPlugInIOOperationThread:
		case kAudioServerPlugInIOOperationCycle:
		case kAudioServerPlugInIOOperationConvertInput:
		case kAudioServerPlugInIOOperationProcessInput:
		case kAudioServerPlugInIOOperationProcessOutput:
		case kAudioServerPlugInIOOperationMixOutput:
		case kAudioServerPlugInIOOperationProcessMix:
		case kAudioServerPlugInIOOperationConvertMix:
		default:
			outWillDo = false;
			outWillDoInPlace = true;
			break;

	};
}

void	SA_Device::BeginIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo)
{
	#pragma unused(inOperationID, inIOBufferFrameSize)
	// CoreAudio gives three timestamps per cycle on the same host clock:
	// mInputTime (capture time), mOutputTime (playback time), mCurrentTime
	// (callback fire time). lead = |mCurrentTime - mInput/OutputTime| in
	// frames. Nominal lead at a P-frame period is P-1 (fence-post): the
	// IOProc fires when the buffer is just complete. Deviations are
	// CoreAudio scheduling stress; near-zero leads risk torn reads.
	//
	// mInputTime is only meaningful during ReadInput, mOutputTime only
	// during WriteMix — sample each side only when its op fires.
	if (gDevice_HostTicksPerFrame <= 0.0) return;

	const SInt64 nowTicks = (SInt64)inIOCycleInfo.mCurrentTime.mHostTime;
	// SafetyOffset shifts mInputTime back / mOutputTime forward by that many
	// frames, so the nominal lead grows from (nframes-1) to
	// (nframes-1 + SafetyOffset). leadJitter stays ≈0 in healthy steady state.
	const SInt64 nominal  = (SInt64)inIOBufferFrameSize - 1 + (SInt64)mSafetyOffsetFrames;
	bool inNearMiss = false, outNearMiss = false;
	if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		SInt64 inLead = (SInt64)((Float64)(nowTicks - (SInt64)inIOCycleInfo.mInputTime.mHostTime)
		                         / gDevice_HostTicksPerFrame);
		mHealthLeadJitter += (UInt64)std::llabs(inLead - nominal);
		if (inLead < 16) inNearMiss = true;
	} else if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
		SInt64 outLead = (SInt64)((Float64)((SInt64)inIOCycleInfo.mOutputTime.mHostTime - nowTicks)
		                          / gDevice_HostTicksPerFrame);
		mHealthLeadJitter += (UInt64)std::llabs(outLead - nominal);
		if (outLead < 16) outNearMiss = true;
	} else {
		return;
	}

	// Count cycles by mCurrentTime change so the emit cadence is in cycles,
	// not ops. nearMiss is per-cycle (either side trips it).
	UInt64 cycleHostTime = inIOCycleInfo.mCurrentTime.mHostTime;
	if (cycleHostTime != mHealthLastHostTime) {
		mHealthLastHostTime = cycleHostTime;
		mHealthCycleCount++;
		if (inIOBufferFrameSize > mHealthMaxNFrames) mHealthMaxNFrames = inIOBufferFrameSize;
	}
	if (inNearMiss || outNearMiss) mHealthNearMiss++;

	// Step 3: publish HAL anchor under a seqlock. Single writer (this thread,
	// this per-cycle path), so the dance is: bump seq to odd, write fields,
	// bump seq to even. The daemon reads seq twice around its snapshot and
	// retries on odd / mismatch.
	UInt64 seq = shmHalAnchorSeq->load(std::memory_order_relaxed) + 1;
	shmHalAnchorSeq->store(seq, std::memory_order_release);
	shmHalAnchorHostTime->store(inIOCycleInfo.mCurrentTime.mHostTime,
	                            std::memory_order_relaxed);
	shmHalAnchorSampleTime->store((UInt64)inIOCycleInfo.mCurrentTime.mSampleTime,
	                              std::memory_order_relaxed);
	shmHalInputReadHead->store((UInt64)inIOCycleInfo.mInputTime.mSampleTime,
	                           std::memory_order_relaxed);
	shmHalOutputWriteHead->store((UInt64)inIOCycleInfo.mOutputTime.mSampleTime,
	                             std::memory_order_relaxed);
	shmHalNFrames->store(inIOBufferFrameSize, std::memory_order_relaxed);
	shmHalSampleRate->store(mSampleRateShadow, std::memory_order_relaxed);
	shmHalAnchorSeq->store(seq + 1, std::memory_order_release);

	// Emit ~every 5s. cycles_per_5s = SR*5/nframes. Skip if nframes unknown.
	UInt64 cyclesPer5s = (inIOBufferFrameSize > 0)
	                   ? (mSampleRateShadow * 5 / inIOBufferFrameSize)
	                   : 0;
	if (cyclesPer5s == 0 || mHealthCycleCount < cyclesPer5s) return;

	JB_LOG_INFO(jb_log_driver(),
		"health cycles=%llu maxBurst=%u nearMiss=%u leadJitter=%llu",
		(unsigned long long)mHealthCycleCount,
		(unsigned)mHealthMaxNFrames,
		(unsigned)mHealthNearMiss,
		(unsigned long long)mHealthLeadJitter);

	mHealthCycleCount = 0;
	mHealthMaxNFrames = 0;
	mHealthNearMiss = 0;
	mHealthLeadJitter = 0;
}

void	SA_Device::DoIOOperation(AudioObjectID inStreamObjectID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
	#pragma unused(inStreamObjectID, ioSecondaryBuffer)
	int streamId = getStreamID(inStreamObjectID);
	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
            ReadInputData(streamId, inIOBufferFrameSize, inIOCycleInfo.mInputTime.mSampleTime, ioMainBuffer);
			break;

		case kAudioServerPlugInIOOperationWriteMix:
			WriteOutputData(streamId, inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime, ioMainBuffer);
			break;
	};
}

void	SA_Device::EndIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo)
{
	#pragma unused(inOperationID, inIOBufferFrameSize, inIOCycleInfo)
}

void	SA_Device::ReadInputData(int streamId, UInt32 inIOBufferFrameSize, Float64 inSampleTime, void* outBuffer)
{
	// No lock: mRingBufferFrameSize, buf_down, and shmReadFrameNumber are set
	// once in _HW_Open / attach_shm before IO starts and never mutated. The
	// teardown path holds mIOMutex in Deactivate to gate this routine off
	// before tearing those down.
    sample_t *RingBuffer = buf_down[streamId];
    std::atomic<uint64_t> *frameNum = shmReadFrameNumber[streamId];

	//	figure out where we are starting
	UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
	UInt32 theStartFrameOffset = theSampleTime % mRingBufferFrameSize;

	//	figure out how many frames we need to copy
	UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
	UInt32 theNumberFramesToCopy2 = 0;
	if((theStartFrameOffset + theNumberFramesToCopy1) > mRingBufferFrameSize)
	{
		theNumberFramesToCopy1 = mRingBufferFrameSize - theStartFrameOffset;
		theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
	}

	//	do the copying (the byte sizes here assume a 16 bit stereo sample format)
    Byte* theDestination = reinterpret_cast<Byte*>(outBuffer);
    if (!mDeviceIsAlive.load(std::memory_order_acquire)) {
        // Daemon stalled - feed silence to the DAW instead of stale ring-buffer
        // contents. DeviceIsAlive=0 has been published; the host should be
        // tearing the device down imminently.
        bzero(theDestination, inIOBufferFrameSize * 8);
    } else {
        memcpy(theDestination, RingBuffer+theStartFrameOffset*2, theNumberFramesToCopy1 * 8);
        if(theNumberFramesToCopy2 > 0)
        {
            memcpy(theDestination + (theNumberFramesToCopy1 * 8), RingBuffer, theNumberFramesToCopy2 * 8);
        }
    }
    frameNum->store(static_cast<UInt64>(inSampleTime) + inIOBufferFrameSize,
                    std::memory_order_release);
}

void	SA_Device::WriteOutputData(int streamId, UInt32 inIOBufferFrameSize, Float64 inSampleTime, const void* inBuffer)
{
	// No lock - see ReadInputData for rationale.
    sample_t *RingBuffer = buf_up[streamId];
    std::atomic<uint64_t> *frameNum = shmWriteFrameNumber[streamId];

	//	figure out where we are starting
	UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
	UInt32 theStartFrameOffset = theSampleTime % mRingBufferFrameSize;

	//	figure out how many frames we need to copy
	UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
	UInt32 theNumberFramesToCopy2 = 0;
	if((theStartFrameOffset + theNumberFramesToCopy1) > mRingBufferFrameSize)
	{
		theNumberFramesToCopy1 = mRingBufferFrameSize - theStartFrameOffset;
		theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
	}

	//	do the copying (the byte sizes here assume a 16 bit stereo sample format)
    const Byte* theSource = reinterpret_cast<const Byte*>(inBuffer);
    memcpy(RingBuffer+theStartFrameOffset*2, theSource, theNumberFramesToCopy1 * 8);
    if(theNumberFramesToCopy2 > 0)
    {
        memcpy(RingBuffer, theSource + (theNumberFramesToCopy1 * 8), theNumberFramesToCopy2 * 8);
    }
    frameNum->store(static_cast<UInt64>(inSampleTime) + inIOBufferFrameSize,
                    std::memory_order_release);
}

#pragma mark Hardware Accessors

CFStringRef	SA_Device::HW_CopyDeviceUID()
{
	CFStringRef theAnswer = CFSTR(kDeviceUID);
	return theAnswer;
}

// Read a UInt32 from config.plist at init time. Falls back to `def` if the
// file/key is missing or unparseable. Runs once in _HW_Open; popen cost is
// irrelevant outside the realtime path.
static UInt32 read_uint_from_plist(const char* key, UInt32 def) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/libexec/PlistBuddy -c 'Print :%s' "
        "'/Library/Application Support/JackBridge/config.plist' 2>/dev/null",
        key);
    FILE* f = popen(cmd, "r");
    if (!f) return def;
    UInt32 val = def;
    if (fscanf(f, "%u", &val) != 1) val = def;
    pclose(f);
    return val;
}

// Base end-to-end latency in frames, excluding JitterFrames. Derived from
// docs/LATENCY-MODEL.md Σ at the documented defaults (48 kHz, pi -p 64,
// netadapter -g 512 -l 2, MTU 1500): everything except T_jf sums to 722.
// Both directions are advertised identically; the DAW adds buffer size and
// SafetyOffset on top, so we do NOT include JitterFrames here.
static constexpr UInt32 kBaseLatencyFrames = 722;
static constexpr UInt32 kDefaultJitterFrames = 192;

void	SA_Device::_HW_Open()
{
    // Initialize shared memory to communicate JackBridge daemon
    int rc = create_shm();
    if (rc < 0) {
        //syslog(LOG_ERR, "JackBridge: Creating shared memory failed (%d)\n", rc);
        Throw(CAException(kAudioHardwareBadDeviceError));
        return;
    }

    if (attach_shm() < 0) {
        //syslog(LOG_ERR, "JackBridge: Attaching shared memory failed (id=%d)\n", instance);
        Throw(CAException(kAudioHardwareBadDeviceError));
        return;
    }

    if (!check_protocol_version()) {
        JB_LOG_ERR(jb_log_shm(),
            "protocol version mismatch - observed %llu, driver built for %d. Reinstall the matching .pkg.",
            (unsigned long long)shmProtocolVersion->load(std::memory_order_acquire),
            JACKBRIDGE_PROTOCOL_VERSION);
        Throw(CAException(kAudioHardwareBadDeviceError));
        return;
    }

    shmSeed->store(1, std::memory_order_relaxed);
    shmSyncMode->store(0, std::memory_order_relaxed);
    mDriverStatus = JB_DRV_STATUS_ACTIVE;
    shmDriverStatus->store(JB_DRV_STATUS_ACTIVE, std::memory_order_release);
    mRingBufferFrameSize = STRBUFNUM / 2;

    // Advertised latency = fixed monitoring-chain base; JitterFrames is
    // surfaced separately via kAudioDevicePropertySafetyOffset so CoreAudio
    // can act on it (scheduling the IOProc earlier) instead of just reporting
    // it. The DAW sums Latency + SafetyOffset, so adding jitter here would
    // double-count it. See docs/LATENCY-MODEL.md.
    UInt32 jitter = read_uint_from_plist("JitterFrames", kDefaultJitterFrames);
    mReportedLatencyInput  = kBaseLatencyFrames;
    mReportedLatencyOutput = kBaseLatencyFrames;
    mSafetyOffsetFrames    = jitter;
    JB_LOG_INFO(jb_log_driver(),
        "device #%u latency=%u frames, safetyOffset=%u frames",
        instance, (unsigned)mReportedLatencyInput,
        (unsigned)mSafetyOffsetFrames);

    JB_LOG_INFO(jb_log_driver(), "device #%u initialized", instance);
}

void	SA_Device::_HW_Close()
{
    mDriverStatus = JB_DRV_STATUS_INIT;
    return;
}

kern_return_t	SA_Device::_HW_StartIO()
{
    JB_LOG_INFO(jb_log_driver(), "StartIO");
    if (mDriverStatus == JB_DRV_STATUS_INIT) {
        return kAudioHardwareNotRunningError;
    }
    mDriverStatus = JB_DRV_STATUS_STARTED;
    shmDriverStatus->store(JB_DRV_STATUS_STARTED, std::memory_order_release);
    gDevice_AnchorHostTime = 0;

    // Re-arm heartbeat tracking. If the daemon's already ticking, this primes
    // mLastDaemonAlive to a recent value so we don't false-positive on the
    // first GetZeroTimeStamp; if it isn't, the staleness threshold gives it
    // ~5 cycles to come up before we mark the device dead.
    mLastDaemonAlive = shmDaemonAlive->load(std::memory_order_acquire);
    mLastDaemonAliveHostTime = mach_absolute_time();
    mDeviceIsAlive.store(true, std::memory_order_release);
    return 0;
}

void	SA_Device::_HW_StopIO()
{
    JB_LOG_INFO(jb_log_driver(), "StopIO");
    mDriverStatus = JB_DRV_STATUS_ACTIVE;
    shmDriverStatus->store(JB_DRV_STATUS_ACTIVE, std::memory_order_release);
	return;
}

UInt64	SA_Device::_HW_GetSampleRate() const
{
	return mSampleRateShadow;
}

kern_return_t	SA_Device::_HW_SetSampleRate(UInt64 inNewSampleRate)
{
    mSampleRateShadow = inNewSampleRate;
	return 0;
}

#pragma mark Implementation

void	SA_Device::PerformConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
	#pragma unused(inChangeInfo)

	//	this device only supports chagning the sample rate, which is stored in inChangeAction
	UInt64 theNewSampleRate = inChangeAction;

	//	make sure we support the new sample rate
	if((theNewSampleRate == 44100) || (theNewSampleRate == 48000))
	{
		//	we need to lock the state lock around telling the hardware about the new sample rate
		CAMutex::Locker theStateLocker(mStateMutex);
		_HW_SetSampleRate(theNewSampleRate);

        //  calculate the host ticks per frame
        struct mach_timebase_info theTimeBaseInfo;
        mach_timebase_info(&theTimeBaseInfo);
        Float64 theHostClockFrequency = (Float64)theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer;
        theHostClockFrequency *= 1000000000.0;
        gDevice_HostTicksPerFrame = theHostClockFrequency / theNewSampleRate;
	}
}

void	SA_Device::AbortConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
	#pragma unused(inChangeAction, inChangeInfo)

	//	this device doesn't need to do anything special if a change request gets aborted
}


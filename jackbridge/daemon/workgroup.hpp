// Workgroup integration for the JackBridge daemon.
//
// On macOS, every Core Audio device exposes an os_workgroup_t via
// kAudioDevicePropertyIOThreadOSWorkgroup ('oswg'). Joining that workgroup
// from a real-time thread tells the kernel scheduler to schedule the thread
// alongside the device's IO thread under a shared deadline — which is exactly
// the relationship between our JACK process callback (here) and the HAL's
// IOProc (in the JackBridge driver). Without it, the scheduler treats our
// JACK thread as independent RT and is free to demote it to an E-core or
// preempt it just before the IO cycle deadline.
//
// Acquisition is on any thread; joining/leaving must happen on the RT thread.
// The daemon driver does NOT need to publish anything — the property is
// defined in AudioHardware.h, not AudioServerPlugIn.h, so coreaudiod owns it.

#ifndef __WORKGROUP_HPP__
#define __WORKGROUP_HPP__

#include <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>

// Look up an audio device by its UID (matches kAudioDevicePropertyDeviceUID),
// then fetch kAudioDevicePropertyIOThreadOSWorkgroup. Returns NULL if the
// device cannot be found or the property is unavailable. Caller owns one
// reference and must os_release it.
//
// Safe to call from any thread; this hits Core Audio APIs and is NOT
// realtime-safe.
os_workgroup_t workgroup_acquire_by_uid(const char* device_uid);

// Join the current (calling) thread to the workgroup. Must be called on the
// thread that will participate (the JACK process callback's thread). Ensures
// the thread has THREAD_TIME_CONSTRAINT_POLICY set first — os_workgroup_join
// returns EINVAL on threads that are not realtime, and that requirement is
// undocumented in the public API. period_ns and computation_ns describe one
// audio cycle; pass the cycle period (1e9 * frames / sample_rate) and roughly
// half that for computation.
//
// `token` must remain live until workgroup_leave_self is called (or the
// thread exits). Returns 0 on success, errno on failure.
int workgroup_join_self(os_workgroup_t wg,
                        os_workgroup_join_token_s* token,
                        uint64_t period_ns,
                        uint64_t computation_ns);

// Symmetric leave; call on the same thread that joined.
void workgroup_leave_self(os_workgroup_t wg, os_workgroup_join_token_s* token);

#endif

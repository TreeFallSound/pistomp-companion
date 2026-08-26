# Presentation cleanup

## Goal

Ship an app that looks intentional on current macOS and has no avoidable asset
or build warnings.

## Work

- Correct the `AppIcon` asset metadata for macOS.
- Ensure the 1024px icon is assigned to a valid app-icon slot and appears in
  the built bundle.
- Resolve the missing `AccentColor` warning by either defining the color or
  removing the unused generated setting.
- Verify the template menu-bar icon and colored badges against both light and
  dark menu bars.
- Verify the icon remains legible at the actual menu-bar size and when dimmed
  for unreachable states.
- Review menu titles, disabled-action behavior, and status-line truncation.

## Acceptance criteria

- Release builds produce no asset-catalog warnings.
- `Info.plist` identifies the app as a menu-bar utility and the built app has a
  working application icon.
- The base icon and amber, hollow-green, solid-green, and red badges are
  readable in light and dark appearances.
- The menu does not expose actions that cannot currently succeed.
- A visual check is recorded from the built `.app`, not from an Xcode preview
  alone.

## Constraints

Keep the status icon lightweight and do not add a window, polling source, or
CoreAudio interaction solely for visual polish.

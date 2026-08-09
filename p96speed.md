# Automated P96Speed Runs

P96Speed 1.2 has no batch or command-line benchmark mode. It can still be run
repeatably through the Amiga MCP input and filesystem commands. The coordinates
below apply to the standard 738x538 P96Speed window positioned at `(0,0)`.
Always take a screenshot after opening a requester because list contents and
the requester's screen position can change with the current mode selection.

## Test Configuration

- Executable: `C:P96Speed`
- Mode: `Radeon 9200: 640x480 16bit PC`
- Test length: 13 seconds
- Number of tests in "Run all tests": 21
- Minimum uninterrupted wait: `21 * 13 + 10 = 283` seconds

Do not send MCP commands while the tests are running. P96Speed occupies the
Amiga long enough that AmigaBridge becomes unresponsive and its TCP connection
may drop. Wait at least 283 seconds before reconnecting. The validated run did
not produce a stable screenshot until about 313 seconds, so allow another 30
seconds if the first reconnect or screenshot fails.

## MCP Procedure

1. Start P96Speed with `amiga_launch` using `C:P96Speed`.
2. Wait about three seconds and call `amiga_list_screen_windows`.
3. If necessary, move the P96Speed window to `(0,0)` with
   `amiga_window_move`. Do not hardcode the window address; it changes between
   runs.
4. Move the injected mouse to the screen origin with a large negative move,
   for example `dx=-2000, dy=-2000`.
5. Select the Preferences tab at approximately `(560,130)`.
6. Open the Screenmode chooser at approximately `(584,230)`.
7. In the observed mode list, select `Radeon 9200: 640x480 16bit PC`, immediately
   above the 32-bit BGRA mode. Use the screenshot rather than a stored Y
   coordinate: observed requesters appeared both at the top of the screen and
   vertically centered.
8. Confirm the mode chooser with its visible OK button. Its observed Y position
   changed from 553 to 427 when the requester moved.
9. Select the Own Machine tab at approximately `(80,130)`. This updates the
   system-information header to the chosen screen mode.
10. Click Run all tests at approximately `(404,522)`.
11. Wait at least 283 seconds without probing AmigaBridge. A suitable host-side
    command is `sleep 283` with a timeout above 293000 ms. If the bridge accepts
    a connection but immediately drops it, wait another 30 seconds and retry.
12. Reconnect to AmigaBridge and take a screenshot to verify all result fields
    are populated.
13. Delete or rename an existing `RAM:P96Speed.txt`. P96Speed appends another
    1889-byte report if the file already exists instead of replacing it.
14. Reset the injected mouse to the origin because benchmark screen switches
    can change its effective position, then click Save results at approximately
    `(105,522)`.
15. The ASL requester defaults to drawer `RAM:` and file `P96Speed.txt`. Click
    its visible OK button; `(158,503)` was valid in the observed layout.
16. Verify `RAM:P96Speed.txt`, then copy it to a unique result name, for
    example:

    ```text
    RAM:P96Speed.txt -> Work:P96Speed_09pipe.txt
    ```

17. Checksum and pull the named file before comparing results.
18. Reboot before collecting focused `p96screen` timings. The long P96Speed
    session temporarily inflated otherwise stable fill/copy timings even after
    its window had closed.

## Useful Gadget IDs

For the validated P96Speed window, `amiga_list_gadgets` reported:

- ID 104: page/tab gadget
- ID 47: Save results
- ID 41: Run all tests
- ID 46: Quit

Coordinates are more useful for the page tabs and ASL requester because their
child controls are not all exposed with descriptive gadget text.

## Validated Automated Run

The first fully automated run was saved as `Work:P96Speed_09pipe.txt`:

- Size: 1889 bytes
- CRC32: `1FCECF1C`
- Mode: 640x480x16
- Test length: 13 seconds

The validated pattern-acceleration result is
`Work:P96Speed_10pattern2.txt`:

- Size: 1889 bytes
- CRC32: `70D67A67`
- RectFill: 2593 operations/second after a focused repeat
- RectFill Pattern: 1387 operations/second

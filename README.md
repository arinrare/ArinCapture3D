# ArinCapture
### Beta version: 1.1.2

## Depth‑From‑Luma (DFL‑S) — A Hybrid Perceptual Depth Estimator
This project uses a custom depth‑from‑luma technique designed for real‑time VR parallax conversion with any window or monitor as the source.
The method combines multi‑sampled luminance analysis, contrast‑adaptive smoothing, mid‑tone stabilisation, multi‑curve depth shaping, and a set of targeted text/UI stabilisers.
The result is a depth field that is:
- stable (minimal shimmer or jitter)
- comfortable (no harsh transitions or flicker)
- responsive (low latency, real‑time)
- visually convincing (strong depth without distortion)
This approach is built from scratch and tuned specifically for desktop/2D‑to‑VR use, with an emphasis on readability, comfort, and natural depth perception.

## Build instructions for VS Code:

- Run x64 Native Tools Command Prompt
- Navigate to project root
- Run:
       .\build 
    OR .\build release
    OR .\build relwithdebinfo generate
- Output will be in /build/Debug for the Debug version
    OR /dist for the release version

- This repo includes a simple packaging script that builds a config, stages the files into `dist/`, and creates a zip using **7-Zip**.

- **If 7-Zip is installed** (`7z.exe` in `C:\Program Files\7-Zip` or `C:\Program Files (x86)\7-Zip`), the script uses it.
- **Otherwise it falls back to PowerShell `Compress-Archive`**.

Notes:
- If a tester sees a missing `VCRUNTIME*.dll` / `MSVCP*.dll` error, they need the **Microsoft Visual C++ Redistributable (x64)** installed.
- For alpha debugging you can also ship the `.pdb` (the script includes it when present).
- Avoid shipping **Debug** builds to testers unless they have a full Visual Studio dev environment: MSVC Debug builds typically depend on the Debug CRT (not provided by the normal VC++ Redistributable).
- There is a **Debug** release build that will work when supplied with all the generated files.

NOTE: The QoL features are pretty raw right now. Read these notes.

## Capture Modes (notes)

- **Monitor Capture** uses DXGI Desktop Duplication.
- **Select Window...** uses Windows Graphics Capture for selection, and then prefers a **DXGI + crop** fallback (capture the monitor containing the selected window, crop to that window's client area) for higher FPS and to avoid WGC throttling when the game is occluded by the output window.
- **Capture Active Window** also prefers **DXGI + crop** (same idea as Select Window) for higher FPS, with WGC as a fallback when needed.
- Where Select Window fails to capture a window, Active Window may work instead

## Virtual Desktop / Quest notes

- The tray option **"Exclude Output Window From Capture"** controls `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.
    - **OFF:** Virtual Desktop / capture apps can see the ArinCapture output window (recommended for headset use).
    - **ON (default):** capture/recording apps will *not* be able to see the output window.

- **Monitor Capture recursion (mirror-in-mirror):** if you capture a monitor and also display the output on that same monitor, you can get an infinite recursion effect.
    - ArinCapture tries to avoid this by defaulting the output window to a *different* monitor when using **Start Capture (Monitor)**.
    - If you still see recursion, use **Cycle Output Monitor** or select **Output Monitor** from the tray.
    - On single-monitor setups, recursion is unavoidable.

## Diagnostics Overlay

- The diagnostics overlay is **OFF by default**.
- Enable it from the tray menu when you want capture/render stats.

## Logs
A log for each session will be generated in the same location as the executable. This is overwritten when ArinCapture is executed, but will persist across multiple capture types within the same session.

## Step By Step

1. start the app
2. Select framerate
2. Select render resolution
3. Select Half SBS
4. Open a game in fullscreen borderless. Make sure the game graphics settings aren't too high (If the GPU is under too much load ArinCapture will drop frames)
5. Alt tab to desktop
6. In the system tray, right clcik the app icon, select Start Capture
    - Monitor will turn your whole desktop into SBS. For this to work, you need to activate it, have a second screen in virtual desktop, then once it is activated use the VD button down the bottom to "Switch Monitors". This will put the active screen on the second monitor and the mirror on the primary monitor, where you can activate SBS. You will need to perfrom all activity on the second monitor, and it will be mirrored to the primary screen as SBS.
    - Select Window will turn one application into SBS (When wanting to convert something like a movie or a TV show, it's best to get the show playing first, and then once it is running capture the window. Turn off SBS in VD to go back to using the mouse.)
    - Active Window you will have 3 seconds to choose the game in the taskbar or alt tab to it, then the SBS view (With the same instructions for application control as Select Window) should pop over the top
7. It should focus the game in the background automatically, and pop the SBS view over the top
8.  Select Half SBS in VD from the bottom display menu (Select below the primary screen) 
9. Play!
    - Note: You can alt-tab to other applications (turn half SBS OFF in VD when you do this, or move your 2D) applications to a second screen in VD like Discord, browser etc. Keep game and app window on the primary screen
    - Note: To get full FPS use Capture Monitor on a second screen. Due to restrictions with how Windows interacts with apps in a window, frames received from a source on the same screen as the ouput will be at a lower framerate
    - After you alt tab away, when coming back select the GAME not the capture window. When you select the game the capture overlay will pop over automatically
    - If you alt tab to the capture window, you can then press ESC to cancel the capture. This is a trick if you want to stop capture without using the tray icon 
    Note: The FPS you select in the app, although it will always process at the rate you set, will be limited by a) The desktop Hz setting in Windows b) The headset's maximum Hz setting c) The capture type (Wikndow Select and Actvie Window are limited to about 48fps due to Micrsoft's SDK. You can get a higher framerate using Monitor Select as outlined in the next section).


## Setting up a single screen to work with full framerate

1. Download and run the following application from Gihub, it is a fork of the Microsft Idd Virtual Display Driver
   https://github.com/VirtualDrivers/Virtual-Display-Driver/releases
2. Run VDD control and install a VDD
3. In Windows display settings, configure the virtual display to be 3840x2160 resolution, and whatever framerate you want your output to be capable of. (Framerate is in advanced graphics settings in Windows display settings). Note to get a true framerate all the way through the pipeline you would need source monitor, capture monitor all set to the desired framerate. Also the headset must be capable of displaying that framerate. For example a Quest 3 can go up to 120Hz (fps).
4. Launch Virtual Desktop. You should see your two monitors in the headset. If not, in the bottom VD menu, click add screen. This will add the windows VDD you added in Step 2
5. Launch your  game 
6. Run ArinCapture and start a Monitor Capture. This should launch the catpure output on the second monitor in VD (Your VDD). Make sure the options Half SBS, Borderless Fullscreen, and Capture Mouse are activated. Select your desired resolution and framerate. 
7. In the bottom VD Menu, select Switch Monitors. This should put the SBS view as your main screen.
8. In the bottom VD Menu select remove screen. This should remove your primary screen (Currently the VD second screen) from your headset
9. In the bottom VD Mene select Half SBS
10. You should now have a single screen in the headset, which is your SBS view running in SBS. You should be able to play a game, wach media, use OBS and a capture card to play a console, all at the full ramerate on a single screen. Also the mouse, keyboard and any gamepad or other input should work.
NOTE: With mouse input for SBS, there is an emulated a cursor based on the source window that is drawn in the capture window. Sometimes this may cause a duplicate cursor especially if the game draws their own cursor. For games that use the mouse, this cannot be avoided and the user will need to manage the mouse by either finding a way to hide it *in the game*, or make it as small as possible so as not to be distracting - on a per game basis.

## Current known bugs
- Windows has a security feature to alert the user to when their screen is being captured, which presents as a yellow/orange border or L bracket on the corner of the captured screen. In Windows 11, the Microsoft 11 SDK allows ArinCapture to override this feature to prvent any unwanted on screen artifacts, but some older builds of Windows 10 do not have this ability. So in some cases you may get an L bracket on the right eye only, in the bottom right. To my knowledge, this is unavoidable. The best workaround is to use Monitor Select, where the L bracket becomes a small black square, which is not as noticeable.

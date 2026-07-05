---
title: "Changelog"
description: "A friendly, chronological record of updates, features, and fixes in Mission Vishwakarma."
weight: 20000
layout: "section"
---

# Project Changelog

Welcome to the **Mission Vishwakarma** changelog! Below is a comprehensive record of all changes, updates, and developments, compiled directly from our project history and styled for quick reading.

## Sunday, July 5, 2026

* **1:07 PM** - fast\_float submodule introduced. `(3561c0b)`
    > properties pane design documentation added.
    > CrockfordBase32.h added to code-core.
* **11:26 AM** - Selection enabled in 3D and 2D environment. `(8e2dd90)`
* **6:21 AM** - Build / veersion cleanups, OpenSSL Size optimizations. `(8caae77)`
* **12:04 AM** - Fable Task 6: Imprvoment logs / telemetry server. `(e223024)`

## Saturday, July 4, 2026

* **3:22 PM** - Interoperability cleanups and reorganizations. `(ec3a917)`
* **2:11 PM** - Fable Task 5: DXF Interoperability Extension. `(8ece1f7)`
* **9:03 AM** - Fable Task 4: Python based extension system. `(a7c3d04)`
    > See design document for details.
* **7:51 AM** - Extension system design finalized: frozen CPython, IPC hardening, storage/signing. `(a50ec38)`
    > Doc updates agreed in review: AppContainer specifics (child-process ban, no
    > network capabilities), ExtensionCommunications.cpp/.h as the single host-side
    > IPC handler, Ed25519-signed extension storage verified on every lazy load,
    > PocketPy recursion ban + 1024-object cap, provenance gating for embedded
    > scripts, developer-mode friction, CPython/PocketPy as git submodules, and the
    > tonight-MVP scope at the top.
    > Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
* **7:51 AM** - PocketPy vendored as amalgamated single-file source. `(ec81847)`
    > Adds PocketPy v2.1.8 (upstream commit a2f16e5f) as its committed amalgamation
    > (pocketpy.c + pocketpy.h) directly in code-external/pocketpy, rather than as a
    > git submodule. This mirrors how SQLite is vendored: no submodule clone or
    > Python amalgamation step in CI, and the exact reviewed bytes are compiled. The
    > amalgamated source is portable C that builds on Windows/macOS/Linux via
    > compile-time #ifdefs. README.md records provenance and the upgrade procedure;
    > LICENSE is PocketPy's upstream MIT license.
    > Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>

## Friday, July 3, 2026

* **7:07 PM** - Fable Task 3: Security hardening of .std parser. `(11d0496)`
* **7:39 AM** - Fable Task 2: Printing option for 2D and 3D enabled. `(7ff97e6)`
* **12:18 AM** - Interoperability with .std file initiated. File reading working now. `(54e750d)`
* **12:17 AM** - Software Installer and Automatic Updater implemented. Fable's 1st commit! `(2bf2a27)`

## Thursday, July 2, 2026

* **9:57 PM** - OpenSSL now being build as part of static build. `(f0c671b)`
* **7:50 PM** - Add OpenSSL 4.0.1 as pinned submodule `(132313b)`

## Monday, June 29, 2026

* **7:03 AM** - Spelling corrections & Agent updates. `(d5be2b2)`

## Sunday, June 28, 2026

* **4:51 PM** - Primitive 3D shapes user modeling enabled. `(455082c)`
* **9:14 AM** - Circle, Ellipses, Arcs are here. `(a6d4e2e)`
* **7:31 AM** - Text in 2D modules now working. `(cde3991)`
* **4:02 AM** - Polygons are here. `(fc621a9)`
* **3:18 AM** - User input enabled for Line & Polyline creation. `(8570415)`
* **2:35 AM** - Line thickness improvements. `(ec5e79a)`
    > Background color improvements.
    > Zooming improvements.

## Saturday, June 27, 2026

* **9:12 PM** - Website and legal improvements. `(4e0d525)`
* **11:51 AM** - Persistance of Page2D and Lines enabled. `(b9a068e)`
* **11:05 AM** - Tree behavior and background color improvements. `(76016e6)`

## Friday, June 26, 2026

* **8:33 PM** - Complete Icon Upgrade. Thanks Gemini ! `(c52d947)`
* **4:27 AM** - Page2D MVP implemented by Codex in 1 night! `(0be9b42)`
* **4:26 AM** - AI Agents guidelines added. Source: https://github.com/multica-ai/andrej-karpathy-skills/blob/main/CLAUDE.md `(8b01c9a)`

## Thursday, June 25, 2026

* **7:17 AM** - GitHub action fix for static build. `(ce22879)`
* **6:52 AM** - Build Optimizations for External Libraries. `(3ee68f5)`
* **5:31 AM** - SVG Icons integrated into User Interface. `(091e557)`
* **5:06 AM** - 2D Rendering architecture 1st Draft. `(c67cfa1)`
* **3:48 AM** - LunaSVG integrations. `(456e539)`

## Wednesday, June 24, 2026

* **10:57 PM** - AI Generated Icons capability. `(0da930b)`

## Monday, June 22, 2026

* **7:48 AM** - Sub-tabs for Scene3D implemented. `(ec0cd72)`

## Thursday, June 18, 2026

* **7:21 PM** - Website update discipline wise. `(35988d3)`
* **6:43 PM** - Download link fixed. Code Sign warning image repositioned. `(7630094)`
* **6:17 PM** - Data Tree implemented. `(2f34e50)`
* **7:34 AM** - Active Branch in Tree selection implemented. `(6880b11)`
* **6:58 AM** - Folder, Scene3D, Scene2D container elements implemented. `(207f4e3)`

## Wednesday, June 17, 2026

* **6:28 AM** - Website Updates. `(f9d8ce0)`
* **4:40 AM** - GitHub Actions fix & Build Optimizations. `(fd6369d)`

## Tuesday, June 16, 2026

* **11:30 AM** - protoc installation in GitHub action runner. `(ec980fe)`
* **9:30 AM** - Remove x86 Reference for fontforge install. `(6d8e10e)`
* **7:56 AM** - Data Tree implemented. `(5d5432f)`

## Monday, June 15, 2026

* **9:27 PM** - Protocol buffer integrated. Initial data save and loading working. `(5bebcec)`

## Sunday, June 14, 2026

* **7:35 PM** - Initial Storage specification and SQLite source checkin. `(6473097)`

## Thursday, June 11, 2026

* **6:56 AM** - Indianization and sidebar improvements. `(4ed9158)`
* **6:40 AM** - SEO Optimization by Google Antrigravity : Gemini 3.5 Flash(Low) `(49eca1b)`
* **6:24 AM** - Website improvements. `(3d23c3a)`

## Sunday, June 7, 2026

* **6:21 AM** - Top UI Scrollbar proportioning fixed. `(3a8feac)`

## Saturday, June 6, 2026

* **10:15 PM** - UI Improvements. Build fix. `(1f15861)`

## Wednesday, June 3, 2026

* **7:48 AM** - MSDF integrated. `(d061770)`
* **1:26 AM** - MSDF font render preparation. Library integrated. `(bbf11d8)`

## Tuesday, June 2, 2026

* **7:59 AM** - Ribbon beautification. `(c98ed65)`
* **7:19 AM** - Button name shortening. `(906ec01)`

## Monday, June 1, 2026

* **7:43 AM** - UI Text shortening. `(d6daecd)`
* **6:50 AM** - Ribbon scroll enabled by Codex. `(e024c06)`
* **12:43 AM** - User Interface layout/buttons list and sequence finalized. `(f652f66)`

## Sunday, May 24, 2026

* **7:18 PM** - Font building enhancement to GitHub runner. `(4b6588d)`
* **4:49 PM** - Tab creation / closing now working. `(37fe405)`

## Saturday, May 23, 2026

* **11:34 PM** - 3D Drawing area reduced with top UI height. `(0778284)`
    > Tab closure / creation buttons added. Yet to work.
* **10:17 PM** - Active Tab now have rounded top rectangle. `(84f6977)`
* **10:09 PM** - checkout: temporary commit for worktree checkout `(87b90a5)`
* **10:01 PM** - UI Improvements. `(e0d3afe)`

## Tuesday, May 19, 2026

* **12:00 AM** - Icon font preliminary work done. Now embeding & compiling. `(cc9e806)`

## Monday, May 18, 2026

* **10:26 PM** - Shaders extracted to .hlsl files. Compiled at build time. `(817e997)`
* **8:03 AM** - Tab swtiching working. `(1eab53d)`
* **7:36 AM** - Initial default tab names provided. `(64f9b58)`

## Sunday, May 17, 2026

* **11:56 PM** - Characterset analysis and improvements. `(d1ec523)`
* **11:36 PM** - Shader Model 5.0 to 5.0 upgrade. `(09f4502)`
* **10:55 PM** - Rounded rectangle buttons, and preliminary work on UI+Multilingual texture. `(a01d02b)`

## Thursday, May 14, 2026

* **9:51 PM** - Action Bar text size fix. Now 2.5mm Always. `(ef90606)`
* **8:50 PM** - UI improvements. Double Click resize fixed. `(906e8c4)`

## Wednesday, May 13, 2026

* **11:52 PM** - Finally top action group buttons looking something good. `(3d51927)`
* **7:42 AM** - Text improvements `(35fac2c)`

## Tuesday, May 12, 2026

* **7:41 PM** - Button position updates. `(38eeafe)`
* **6:37 PM** - Removed defaultWidthPX from User Interface controls. `(634fbc5)`
* **7:42 AM** - Compiled translation file usage started. `(54829ff)`

## Monday, May 11, 2026

* **1:15 AM** - Progress towards UI `(93e4a96)`

## Saturday, May 9, 2026

* **6:07 PM** - Advances in UI Translation texts. `(a5f06a1)`

## Friday, May 8, 2026

* **9:06 PM** - Upgraded dependency to respective latest release versions. `(eb1a732)`
* **8:34 PM** - Windows aerial.ttf replaced with embedded Noto Sans Regular font. `(97851ab)`
    > 1st Commit Fully authored by Codex!
* **12:22 AM** - Noto Font files / folder added. `(2760530)`

## Thursday, May 7, 2026

* **12:43 AM** - Release build freetype linking fixed. `(3669f6b)`

## Wednesday, May 6, 2026

* **11:46 PM** - Texture processing crash fixed. `(4f515b4)`

## Tuesday, April 14, 2026

* **12:27 PM** - User Interface development work in progress. Crashing. `(a2a562a)`

## Friday, March 27, 2026

* **6:41 PM** - Basic top menu bars prepared. `(427d48c)`

## Tuesday, March 17, 2026

* **11:12 PM** - Language list finalized. `(6368caa)`
* **10:22 PM** - Initial User interface (tab only) implementation & integrations. `(9b89cbf)`

## Sunday, March 15, 2026

* **11:09 PM** - User Interface design / planning started. `(976e2ad)`
* **2:25 AM** - Command deduplication, VRAM Memory optimization and other bug fixes. `(7922cad)`

## Saturday, March 14, 2026

* **6:52 PM** - Simplified RCU publish code to process 1 tab at a time. Moved resize logic to render thead. `(aae21b0)`

## Tuesday, March 10, 2026

* **7:30 PM** - GPU VRAM Memory leak fix. `(47f97e8)`
* **3:59 AM** - Misc bug fixes and code quality improvements. `(5760244)`

## Monday, March 9, 2026

* **12:09 AM** - GPU VRAM Pages Read Copy Update mechanism implemented. Monitor migration movded from main UI thread to render thead. `(05781d5)`

## Monday, March 2, 2026

* **7:18 PM** - Code Cleanups. `(45fb4cd)`

## Sunday, March 1, 2026

* **6:44 PM** - Compiler warning cleanups. `(84b2476)`
* **4:56 PM** - Jumbo buffer replaced with immutable VRAM pages per tab architecture. Initial implementation. Code cleanups and some optimizations. `(f2c2a7f)`

## Monday, February 23, 2026

* **11:57 PM** - Initial preparations for ExecuteIndirect . PipelineStateObject and RootSignature moved from PerWindow to PerTab. `(91d0534)`

## Sunday, February 22, 2026

* **7:33 PM** - Per Object Commited resource to Jumbo Buffer committed resource implemented. Sync issues / Double buffering to be fixed. `(3bc3fc5)`

## Wednesday, February 18, 2026

* **12:36 AM** - Executable file descreption shortened. `(01255c0)`
* **12:32 AM** - Fixed basic 3d geometry elements. Removed common vertex sharing between 2 adjescent surfaces. `(b5f9011)`

## Tuesday, February 17, 2026

* **11:17 PM** - Enable dark mode for website. `(60d2456)`
* **1:23 AM** - Website improvement. `(bbffa0b)`
* **1:06 AM** - Render to texture implemented. `(dcf1fb8)`

## Sunday, February 15, 2026

* **11:14 PM** - Readme beautification ! `(f3ef18c)`
* **11:01 PM** - Fix LOC badge. `(d2af457)`
* **10:30 PM** - LOC Badge update. `(f4f6da5)`
* **9:20 AM** - Website improvements. `(fe8c306)`
* **8:10 AM** - 1. WM\_SIZE implemented. `(e128990)`
    > 2. Graphics adapter selection based on VRAM size fixed.
    > 3. objectsOnGPU moved to per tab structure from global.

## Saturday, February 14, 2026

* **4:56 PM** - std::vector changed to fixed size array for global variables allTabs and allUIWindows. `(a67c6b7)`

## Wednesday, February 11, 2026

* **11:57 PM** - Zoom/Pan/Rotate bug fixes. Monitor array changed to static size with 16 Nos. max. `(e46619d)`

## Sunday, February 8, 2026

* **7:19 PM** - Misc Code quality improvements `(848b904)`

## Saturday, February 7, 2026

* **10:59 AM** - Pan / Zoom / Scroll compleated. `(776ff04)`
* **5:48 AM** - Zoom In / Out now working. Camera now managed by Engineering. `(3ddc385)`

## Monday, February 2, 2026

* **8:15 PM** - Added abiliy to embed source files into website. Ex. graphics.md `(e0554ff)`

## Thursday, January 29, 2026

* **6:50 AM** - commandQueue missmatch handling / safeguard added. `(897c1bf)`
* **6:27 AM** - Thread shutdown partially fixed. Some cleanup. `(ffffe94)`
* **4:23 AM** - mend `(b8b3113)`
* **4:16 AM** - Shaders updated to process normals. Color format reduced from float32 to float16 `(6366058)`

## Wednesday, January 28, 2026

* **5:55 AM** - Normals added to Vertex. Shaders yet to update. `(40a644d)`

## Monday, January 26, 2026

* **7:23 PM** - mend `(857cfb9)`
* **7:18 PM** - Website Updates. `(3a4e4f9)`

## Sunday, January 25, 2026

* **7:41 PM** - Nightly build Github action added. `(b5d3d64)`
* **6:00 PM** - Made logo internal resource. Now .exe running directly. `(b619e7f)`
* **11:50 AM** - GPU Engine roadmap set. Time to get GPU Engine productionr ready. `(b968d56)`

## Monday, January 12, 2026

* **12:15 AM** - Reorganization. Moved .sln file to root of folder. `(c735ecd)`

## Tuesday, December 30, 2025

* **3:23 AM** - Spell checks ;) `(85ae0b0)`
* **3:05 AM** - Initial Multi-threaded, Multi-monitor, Multi-tabs support added. Unicode Upgrade. `(49720ea)`

## Tuesday, October 7, 2025

* **11:32 PM** - Cleanup and reorganization. `(58ca620)`

## Sunday, October 5, 2025

* **11:09 PM** - Reorganization and cleanup. `(e3b0a17)`
* **1:27 AM** - Now all basic shapes rendering. Social Links added to website. `(25353ec)`

## Saturday, October 4, 2025

* **10:33 PM** - Major refactoring. Copy and Render thread introduced. Graphics code moved out to main to respective places. `(cd9019c)`

## Monday, September 1, 2025

* **11:49 PM** - System Requirement and Legal updates. `(2927ec2)`

## Monday, August 11, 2025

* **12:46 AM** - Optional64 class work in progress. Learning Template Meta Programming. `(fa0d9a9)`

## Tuesday, August 5, 2025

* **12:41 AM** - Improvements to CPU RAM Manager. Preliminary preparation to process user inputs. `(f4d3e30)`

## Monday, July 28, 2025

* **11:10 PM** - Vertex generation added to  per frame running populate command list. `(1a270f3)`
* **1:52 AM** - Line of code badge added. `(daf7a1d)`
* **1:32 AM** - Multi threading code initial draft introduced. Yet to be implimented. `(b5527e8)`

## Wednesday, July 23, 2025

* **11:58 PM** - Initial engineering data models addded. Minor fixes to RAM manager. `(29627d1)`

## Sunday, July 20, 2025

* **11:23 PM** - Code compaction and explanatory commentary added. `(3768395)`
* **2:06 AM** - Initial support for Multi monitor detection and support. `(018a550)`
* **12:07 AM** - 2D Triangles upgraded to 3D Pyramid + Rotation. Let's go. `(be97d3c)`

## Saturday, July 19, 2025

* **4:49 AM** - One triangle to many triangles. `(2d2a034)`
* **4:11 AM** - Spellcheck. Comment Cleanup and Compaction. `(25144a4)`
* **3:30 AM** - Index Buffer Introduced. `(b7923d8)`

## Friday, July 18, 2025

* **1:22 AM** - Startup maximized. `(d3751b1)`

## Thursday, July 17, 2025

* **11:13 PM** - 1st Refactoring. GPU Management moved from main.cpp to a new file. `(2d9e380)`

## Thursday, July 10, 2025

* **1:26 AM** - libpng and zlib now compiling directly from .h & .c file. Remaining traces of freetype library removed. `(cdba96c)`

## Wednesday, July 9, 2025

* **11:31 PM** - Updated submodule libpng-code to tag v1.6.50 and change from sourcefore repositary to github repositary. `(74f16be)`

## Sunday, June 29, 2025

* **10:53 PM** - Privacy policy added and additional clarification to legal page provided. `(8753d2d)`
* **8:32 PM** - Marketing permission added to EULA. `(c597425)`

## Sunday, June 22, 2025

* **12:04 AM** - Update End User License Agreement.md `(9e0b6c5)`

## Wednesday, June 4, 2025

* **12:02 AM** - More DirectX12 explanatory comments added. Some cleanups. Many GDI Commands commented out now. `(3942cd3)`

## Tuesday, June 3, 2025

* **9:43 PM** - US Export Control explanation link added to legal.md `(87b9f21)`

## Tuesday, May 27, 2025

* **7:00 AM** - More DirectX12 added. Most of GDI Commands removed. Some still pending. `(4b6b136)`

## Wednesday, May 7, 2025

* **12:27 AM** - Spelling check graphics.md `(446fe9f)`
* **12:17 AM** - graphics.md moved to correct folder. `(9e05cc1)`
* **12:09 AM** - Create graphics.md `(dbf93b0)`
    > Graphics API page in software part of website created.

## Sunday, April 20, 2025

* **11:19 PM** - DirectX 12 introduced. Hello Triangle rendering now. GDI shall be removed latter. `(ed22cfd)`
* **10:52 PM** - DirectX12 Headers repsitary submodule added and freetype submodule moved to latest release to resolve min cmake error. `(1cf46fe)`

## Saturday, April 19, 2025

* **10:32 PM** - windows folder api-direct3d renamed to api-directx12 since 3d is just a part of direcX family. `(2623f1b)`

## Thursday, April 17, 2025

* **10:13 AM** - Minor motivational edit. `(2e949b3)`

## Friday, April 11, 2025

* **12:33 AM** - Replace incorrect total RAM in the world estimate with largest Super Computer estimate. `(dcc4446)`
* **12:17 AM** - New article on IDs added. `(573146d)`

## Tuesday, March 4, 2025

* **11:04 PM** - Added thought experiments while on train ! `(e264bf7)`

## Tuesday, February 25, 2025

* **10:58 PM** - Created Software wishlist `(b106aa7)`
    > More of a milestones to achieve. Let's Go.

## Monday, February 24, 2025

* **11:07 PM** - Website added with multiple pages and navigation pane. Now we can build the website directly on github. `(10e0d4f)`

## Monday, January 6, 2025

* **11:33 PM** - Signup Link (Google Form) added to our homepage. `(6a1b423)`

## Sunday, December 29, 2024

* **9:47 PM** - High DPI awareness set to per Monitor. However, application not updated to respond dynamically. `(b40c6c9)`
* **9:26 PM** - Build file metadata added. Version number shall be automated latter. `(335f649)`
* **9:06 PM** - Application Binary Icon configured. `(8be3cad)`

## Saturday, December 28, 2024

* **12:05 PM** - x86 removed. `(8bec984)`
* **11:54 AM** - windows.h moved to pre-compiled headers. Basic UI header preparations done. `(ec68e77)`

## Saturday, August 24, 2024

* **3:34 PM** - Our Website using static site generator HUGO implemented. `(a87a0fd)`

## Sunday, July 21, 2024

* **10:33 PM** - Dummy User interface created for Urjalekh. `(bc82a70)`

## Thursday, July 4, 2024

* **10:52 PM** - libpng integration completed. Now displaying our logo in application. `(15faca1)`

## Wednesday, July 3, 2024

* **1:04 AM** - Logo Finalized. `(59f0b0e)`

## Thursday, June 27, 2024

* **10:41 PM** - zlib and pnglib submodule added. Yet to configure properly. `(a60cd72)`

## Friday, June 21, 2024

* **6:33 AM** - We drawn our first 100 straight lines ! Preparatory work for Mouse Event handling. `(a14fdb8)`

## Wednesday, June 19, 2024

* **5:30 AM** - Text rendring using FreeType library compleated. `(8419217)`

## Friday, June 14, 2024

* **7:35 AM** - Freetype library submodule added. Our 1st Dependency. Yet to be configured. `(e2e7002)`

## Sunday, June 9, 2024

* **11:49 PM** - Flatten the windows folder heirarchy. `(06a3ed0)`
* **11:45 AM** - Milestones added. Now in active development again. `(8cdaba8)`

## Tuesday, March 19, 2024

* **2:09 AM** - Updated License and Readme. `(7199985)`

## Wednesday, November 15, 2023

* **10:05 AM** - Folder reorganization based on graphics api. etc. `(ce1c24d)`

## Saturday, September 30, 2023

* **11:06 PM** - Hello World Done. `(73d9727)`

## Thursday, August 17, 2023

* **10:17 PM** - Merge branch 'main' of https://github.com/ramubhai/Vishwakarma into main `(722f20a)`
* **10:17 PM** - Readme Updates and Issue classifications added. `(b03ce8b)`

## Tuesday, January 24, 2023

* **7:08 PM** - Update README.md `(4f59acd)`

## Tuesday, January 17, 2023

* **7:55 PM** - Reduced scope. Pivoting to educational purpose. Licensing updated. `(5fa9ba7)`

## Saturday, January 14, 2023

* **9:37 PM** - Create basis windows desktop application using Microsoft walkthough. `(958b6b8)`
* **9:36 PM** - gitignore file appended with Visual Studio template. `(8e7d506)`
* **8:19 PM** - Basic folder structure of the code added. `(e4c84d6)`

## Thursday, January 12, 2023

* **9:26 PM** - Added AGPL license and contributor terms. `(08076ee)`

## Sunday, January 8, 2023

* **7:32 PM** - Update LICENSE.md `(871b0b5)`
* **7:26 PM** - Streamlined & organized the Readme. `(70445dc)`
* **6:32 PM** - Update LICENSE.md `(746fb1d)`

## Tuesday, December 20, 2022

* **9:51 PM** - Readme formatting and minor update. `(ee4f866)`

## Sunday, November 20, 2022

* **11:07 AM** - Create LICENSE.md `(d36f164)`
* **10:54 AM** - Create .gitignore `(dcb5f9a)`
    > GITHUB suggested .gitignore file for C++ repositories.

## Friday, November 18, 2022

* **11:29 PM** - जय गणेश `(ae11aa4)`
    > India's 1st privately developed rocket Mission Prarambh (means The Beginning) launched today. That was too much of inspiration to overcome my procrastination. Friday Night Fun :-).

## Welcome, स्वागत, வரவேற்பு, സ്വാഗതം, స్వాగతం, ಸ್ವಾಗತ, خوش آمدید, स्वागत आहे 

Vishwakarma, named after the God of Architectural Creation, is an self-educational *Engineering* software. To help myself (Ram Shanker) learn various engineering approaches, algorithms, technology trends etc. It is NOT a validated or commercial software. You are free to look around and learn. My current explorations include (A) Pressure Vessel, (B) Process Piping and (C) Structural Engineeing. This software is can not open/interoperate with any commercial software generated files !


## Engineering Scope. 
As I learn fundamental of engineering concepts, more and more exclusions shall shift to Inclusion as years progress. Usually, higher in the list, higher the priority!

### Broad Scope of the software.
2D Drafting  
Piping & Instrumentation Diagrams (Intilegent 2D)  
Pressure Vessels / Equipment Modeling and Anylysis  
Shell Development Drawing Extraction (2D)  
Process Piping Modeling and Anylysis  
Isometrics Extraction (2D)  
ASME / API Code checking  
Heater Modeling and Analysis  
Structural Modeling and Anylysis. Little Architecture modeling  
Structural Code Checking  
Structrual GA Drawings (2D)  
Local File Support. Data Server Support  
Collaboration Support   
User Manual / Documentation  
Developer Manual / Documentation  

### Exclusions  
Training Curriculum.  
Web API.  
Response Spectrum Analysis.  
Cluster / Remote server analysis.  
Interoperability with other softwares.  
Connection Design.  
Foundation Design.  
PEB Design.  
Non-isotropic Material.  
Reinforcement Detailing.  
Fabrication Drawing.  
Pre-stressing tendons.  
Composite Section.  
Architectural Rendering.  

## Philosophy
Some philosophy / guiding principle of this project. At least during the first two years of development.  
* This software is supposed to be Jack of All trades, Master of None.  
* This software is a freeware. Available freely for everyone without any warranty. Not for commercial use.  
* Get the engineering part correct, leave graphics part for latter, when we have broader open source community.  

### Our Inspirations of Open Source Softwares

Following technology choices are under consideration. In no particular order. I take inspiration from other free & open source softwares.  

* Chromium : For Speed. Speed & Responsiveness of our software is very high priority. As much as accuracy of calculation. C++ 17.   
* SQLite : For robustness and care. This is the database we use for local file storage.
* Postgres SQL : For network based data storage and Multi-User collaboration.  
* Blender : For 3D Rending.  

  
### Technology Choices:
* Protocol Buffer : For data class modeling and storage in database.  
* Riben Interface : All the latest desktop softwares use to it.  
* IFC Format : Open file format for interoperability with other software.  
* HTML Format & PDF Format : For design report generation.  
* ULID : All our objects / data-entries shall be identified by ULID. This is to assist easy copy-paste / merging / integration / interoperability between models.  
* All strings are UTF-8 encoded UNICODE strings.  
* No Commercial database is planned to be officially supported.  

## Github reported issue classification.
| Color  | Tag        | Description|
| ------ | ---------- | ---------------------- |
| Green  | Solved     | Required code changes done.|
| Green  | Closed     | Requested clarification provided to user in the issue itself. No change in code/docs. |
| Green  | Documented | Documentation improved |
| Red    | CodeRed   | All decks on the  table. Roadmap paused. Only for data corruption / synchronization issue. |
| Orange | Analysis   | Errors in Analysis. |
| Orange | Design     | Errors in code-checking. |
| Yellow | Graphics | Related to issues with display on the screen. |
| Yellow | Reports  | Design reports printing error, without inaccuracy in analysis & design. |
| Yellow | Usability| Minor UI changes required to improve the usability of the software. |
| Blue   | FeatureRequest| Reported issue is a feature  request. May be incorporated in future.|
| Black  | ReviewPending | Freshly reported issues. Auto assigned by the Bot.|
| Black  | NeedMoreInfo  | Difficulty in reproduction. Need more information from the user. |
| Gray   | WontFix | The reported bug shall not be acted upon. |
| Gray   | TooOld  | Too old to be acted upon. Automatically assigned after 6 month of inactivity. |

### General Coding Guidelines
* This being a new Green-Field software project, we intend to avoid too much old / legacy technology. We intend to support LATEST and LATEST-1 version of all operating systems. Accordingly we shall support Windows 10 / 11 onwards only. MAC OS 13 (latest only). Linux Ubuntu 22.04 (latest only). Android 12 / 13, iOS 16 (latest only).  

* Few things, we intend to Re-Invent intentionally, for educational, responsiveness and fine-grained control over UI. Attempt shall be made to avoid as much dependencies. I understand this shall slowdown the development, but we are taking this decision for future velocity.  

* I am restring the initial scope to ensure focus. However some architectural decisions can be made considering future scope. Nevertheless, it is our explicit goal NOT to be interoperable with other commercial softwares.  

* Data compression is prioritized, even at the expense of ignoring programmatic flexibility. 

## About Developers 
This project is being developed by me (Ram Shanker) in my personal time. I am employed by Engineers India Limited. For Life. :-) We are one of the best Refinery & Petrochemical consultancy in the world. If you wish to setup a Grass-root oil refinery, or modernize an existing one, get in touch with EIL. https://engineersindia.com/

Welcome, स्वागत, வரவேற்பு, സ്വാഗതം, స్వాగతం, ಸ್ವಾಗತ, خوش آمدید, स्वागत आहे, 

Vishwakarma, named after the Hindu God of Architectural Creation, is an upcoming, currently hypothetical *Structural Engineering* software. It is a FREE software, being developed by Civil Engineers, for Civil Engineers. You are welcome to contribute your expertise. Currently we are under Technology Selection, Scope demarcation and Proof of Concept stage.

********************************************************************************  
Engineering Scope. More and more exclusions shall shift to Inclusion as years progress. Usually, higher in the list, higher the priority!  
  
Inclusions:  
Concrete and Steel 3D Frame Analysis.  
Fixed Support, Pin Support, Spring Supports.  
Local File Support. Data Server Support. Collaboration Support.  
Static Analysis on Local PC.  
IS456 Design Checking.  
IS800:2007 Limit State Design Checking.  
General Arrangement Drawing.  
User Manual / Documentation.  
Developer Manual / Documentation.  
  
Exclusions:  
Training Curriculam.  
API.  
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
  
********************************************************************************  
Some philosophy / guiding pricinple of this project. At least during the first two years of development.  
This software is supposed to be Jack of All trades, Master of None.  
Get the engineering part correct, leave graphics part for latter, when we have broader opern source community.  
Data compresson is prioritized, even at the expense of ignoring programatic flexiblity.  

********************************************************************************  
On Computer technology / For software developers.  

Following technology choices are under consideration. In no particular order. We take inspiration from other free & open source softwares. Take as much inspiration from them as you can.  

Chromium : For Speed. Speed & Responsivness of our software is very high priority. As much as accuracy of calculation. C++ 17.   
SQLite : For robustness and care. Our License is also inspired from this software.  
Postgres SQL : For network based data storage and Multi-User collorabation.  
GoDot Engine : Graphics inspiration. Proof of Concept may use this engine for rendering 3D models. Latter on we will switch to native platforms.  
Blender : For 3D Rending.  

  
Other Technology Choices:  
Protocol Buffer : For data class modeling and storage in database.  
Riben Interface : All the latest professional software are shifting to it.  
IFC Format : Open file format for interoperability with other software. If other softwares support some API, we may use it.  
PDF Format : For design report generation.  
UUIDv7 : All our objects / data-entries shall be identified by UUIDv4. This is to assist easy copy-paste / merging / integration / interoperability between models.  
All strings are UTF-8 encoded strings.  
No Commercial database is planned to be officially supported. Not in initial 5 years.  


This being a new Green-Field software project, we intend to avoid too much old / legacy technology. We intend to support LATEST and LATEST-1 version of all operating systems. Accordingly we shall support Windwos 10 / 11 onwards only. MAC OS 13 (latest only). Linux Ubuntu 22.04 (lastest only)   Android 12 / 13, iOS 16 (latest only).  


Few things, we intend to Re-Invent intentionally, for responsiveness and finegrained control over UI. Attempt shall be made to avoid as much dependencies. I understand this shall slowdown the development, but we are taking this decision for future velocity.  


We understand that even though we intend to be end-to-end software for structural engineers, we are restring the initial scope to ensure focus. Howerver some architectural decisions can be made considering future scope. Nevertheless, it is our explicit goal to be interoperable with other commercial softwares. i.e Offically support it.  


********************************************************************************  
PS: This project is being developed by Ram Shanker in his personal time. Currently awaiting Management Go ahread.

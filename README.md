## Welcome, स्वागत, வரவேற்பு, സ്വാഗതം, స్వాగతం, ಸ್ವಾಗತ, خوش آمدید, स्वागत आहे 

Vishwakarma, named after the God of Architectural Creation, is a self-educational *Engineering* software. To help myself (Ram Shanker) learn various engineering approaches, algorithms, technology trends etc. It is NOT a validated or commercial software. You are free to look around and learn. My current explorations include (A) Pressure Vessel, (B) Process Piping and (C) Structural Engineeing. This software can not open/interoperate with any commercial software generated files !


## Engineering Scope. 
As I learn fundamental of engineering concepts, more and more exclusions shall shift to Inclusion as years progress. Usually, higher in the list, higher the priority!

### Inclusions
Structural Modeling.  
Collaboration Support.  
Pressure Vessels / Equipment Modeling.  
Process Piping Modeling.  
2D Drafting.  
Fixed Support, Pin Support, Spring Supports.  
Concrete and Steel 3D Frame Analysis.  
Local File Support. Data Server Support.  
Structural General Arrangement Drawing.  
User Manual / Documentation.  
Developer Manual / Documentation.  

### Limited development
P & IDs.  
Pressure Vessel Analysis.  
Piping Analysis.  

### Exclusions
IS456 Design Checking.  
IS800:2007 Limit State Design Checking.
ASME Code Checking.  
Training Curriculam.  
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
Some philosophy / guiding pricinple of this project. At least during the first two years of development.  
* This software is supposed to be Jack of All trades, Master of None.  
* This software is a freeware. Not for commercial use.  
* Get the engineering part correct, leave graphics part for latter, when we have broader opern source community.  

### Our Inspirations Open Source / Free Softwares

Following technology choices are under consideration. In no particular order. I take inspiration from other free & open source softwares.  

* Chromium : For Speed. Speed & Responsivness of our software is very high priority. As much as accuracy of calculation. C++ 17.   
* SQLite : For robustness and care. This is the database we use for local file storage.
* Postgres SQL : For network based data storage and Multi-User collorabation.  
* Blender : For 3D Rending.  

  
### Technology Choices:
* Protocol Buffer : For data class modeling and storage in database.  
* Riben Interface : All the latest desktop softwares use to it.  
* IFC Format : Open file format for interoperability with other software.  
* HTML Formt & PDF Format : For design report generation.  
* ULID : All our objects / data-entries shall be identified by ULID. This is to assist easy copy-paste / merging / integration / interoperability between models.  
* All strings are UTF-8 encoded UNICODE strings.  
* No Commercial database is planned to be officially supported.  


### General Coding Guidelines
* This being a new Green-Field software project, we intend to avoid too much old / legacy technology. We intend to support LATEST and LATEST-1 version of all operating systems. Accordingly we shall support Windwos 10 / 11 onwards only. MAC OS 13 (latest only). Linux Ubuntu 22.04 (lastest only). Android 12 / 13, iOS 16 (latest only).  

* Few things, we intend to Re-Invent intentionally, for educational, responsiveness and finegrained control over UI. Attempt shall be made to avoid as much dependencies. I understand this shall slowdown the development, but we are taking this decision for future velocity.  

* I am restring the initial scope to ensure focus. Howerver some architectural decisions can be made considering future scope. Nevertheless, it is our explicit goal NOT to be interoperable with other commercial softwares.  

* Data compresson is prioritized, even at the expense of ignoring programatic flexiblity. 

## About Developers 
This project is being developed by me (Ram Shanker) in my personal time. I am employed by Engineers India Limited. For Life. :-) We are one of the best Refinery & Petrochemical consultancy in the world. If you wish to setup a Grass-root oil refinery, or modernize an existing one, get in touch with EIL. https://engineersindia.com/

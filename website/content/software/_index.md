---
title: "Software Engineering"
weight: 100000
layout: "section"
---

## Philosophy
Some philosophy / guiding principle of this project. At least during the first two years of development.  
* This software is supposed to be Jack of All trades, Master of None.  
* This software is a freeware. Available freely for everyone without any warranty.  
* Get the engineering part correct, leave graphics part for latter, when we have broader open source community.  

### Our Inspirations of Open Source Softwares

I take inspiration from other free & open source softwares.  

* Chromium : For Speed. Speed & Responsiveness of our software is very high priority. b  
* SQLite : For robustness and care. This is the database we use for local file storage.
* Postgres SQL : For network based data storage and Multi-User collaboration.  
* Blender : For 3D Rending.  

  
### Technology Choices:
* Protocol Buffer : For data class modeling and storage in database.  
* Ribbon Interface : All the latest desktop softwares use to it.   
* HTML Format & PDF Format : For design report generation.  
* 64bit Local IDs / ULID.  
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
* This being a new Green-Field software project, we intend to avoid too much old / legacy technology. We intend to support LATEST and LATEST-1 version of all operating systems. Accordingly we shall support Windows 10 / 11 onwards only. MAC OS 13 (latest only). Linux Ubuntu 24.04 (latest only). Android 13 / 14, iOS 17 (latest only).  

* Few things, we intend to Re-Invent intentionally, for educational, responsiveness and fine-grained control over UI. Attempt shall be made to avoid as much dependencies. I understand this shall slowdown the development, but we are taking this decision for future velocity.  

* I am restricting the initial scope to ensure focus. However some architectural decisions can be made considering future scope. Nevertheless, it is our explicit goal NOT to be interoperable with other commercial softwares.  

* Data compression is prioritized, even at the expense of ignoring programmatic flexibility. 
---
title: "User Interface"
weight: 100106
---
We follow the standard ribbon interface for UI. Further, supporting all indian scheduled languages is must.
This will als enable us to support international languages in future. Please refer below actual code files for user interface design specifications. We have baseline strings say N numbers in English. Each string can have a corresponding language translation or if the language translation is empty string, than the corresponding english text shall be followed. All translations are stored in UserInterface-Text.h file.

Supporting all Indian languages is mostly a data organization + text shaping problem, not a rendering problem. Renderer will just draw glyphs; the system around it decides which string to show.

### Translation Document
{{< codefile src="code-core/UserInterface-TextTranslations.h" >}}

### User Interface Design Document and Implementation!
{{< codefile src="code-core/UserInterface-DirectX12.h" >}}
{{< codefile src="code-core/UserInterface-DirectX12.cpp" >}}

### Miscellaneous philosophy:
Renderer must support these scripts:

| Script     | Languages                                                                     |
| ---------- | ----------------------------------------------------------------------------- |
| Latin      | English, German, French, Spanish, Portuguese, Polish, Dutch, Swedish, Italian |
| Cyrillic   | Russian, Ukrainian                                                            |
| CJK        | Chinese, Japanese                                                             |
| Hangul     | Korean                                                                        |
| Arabic     | Urdu                                                                          |
| Indic      | Hindi, Bengali, Telugu, Tamil, etc                                            |
| Thai       | Thai                                                                          |
| Vietnamese | Latin + diacritics                                                            |

Recommended Font Families:
NotoSans-Regular
NotoSansCJK-Regular
NotoSansDevanagari
NotoSansTamil
NotoSansTelugu
NotoSansThai
NotoSansArabic
NotoSansHebrew

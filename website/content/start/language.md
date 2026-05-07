---
title: "Supported Languages"
weight: 11122
---
Engineering is more fun and personal when done in mother tongue.

It is our goal that we cover >95% of engineers all over the world.

See our basic language handling source code below to have an idea of which languages this software intends to support. All translators are welcome to contribute once we have appropriate mechanism setup for this.
Initial translation is performed autonomously by AI.

We are thankful to Google for providing all the Font files for free. Here are the font files with their respective language coverage.

1. Global Latin & Cyrillic Base  
NotoSans-VariableFont_wdth,wght.ttf  
NotoSans-Italic-VariableFont_wdth,wght.ttf  

Covers: English, Spanish, Portuguese, Russian, French, Indonesian, German, Vietnamese, Turkish, Italian, Polish, Ukrainian, Dutch, Filipino, Swedish, Czech, Hungarian.  

Western engineering conventions heavily use italics for variables (e.g., x, y, z) and scientific nomenclature.  

2. Complex Arabic / Perso-Arabic Scripts  
NotoSansArabic-VariableFont_wdth,wght.ttf (Arabic, Persian (Farsi), Urdu, Kashmiri.)  

3. Indian Subcontinent  
NotoSansDevanagari-VariableFont_wdth,wght.ttf (Hindi, Marathi, Nepali, Sanskrit, Maithili, Sindhi, Dogri, Konkani, Bodo)  
NotoSansBengali-VariableFont_wdth,wght.ttf (Bengali, Assamese)  
NotoSansTamil-VariableFont_wdth,wght.ttf (Tamil)  
NotoSansTelugu-VariableFont_wdth,wght.ttf (Telugu)  
NotoSansGujarati-VariableFont_wdth,wght.ttf (Gujarati)  
NotoSansGurmukhi-VariableFont_wdth,wght.ttf (Punjabi)  
NotoSansKannada-VariableFont_wdth,wght.ttf (Kannada)  
NotoSansMalayalam-VariableFont_wdth,wght.ttf (Malayalam)  
NotoSansOriya-VariableFont_wdth,wght.ttf (Odia)  
NotoSansOlChiki-VariableFont_wght.ttf (Santali)  
NotoSansMeeteiMayek-VariableFont_wght.ttf (Manipuri / Meitei)  

Typography Note: Notice there are no -Italic variable files for these. That is historically accurate; Indic scripts traditionally do not use slanted italics for emphasis (they use weight changes instead). We are not missing anything.  

4. Southeast Asia  
NotoSansThai-VariableFont_wdth,wght.ttf (Thai.)  

5. Mathematics and Technical Symbology  
NotoSansMath-Regular.ttf  
NotoSansSymbols-VariableFont_wght.ttf  
NotoSansSymbols2-Regular.ttf  

Everything CAD users will ever need to dimension a drawing, write an equation, or denote geometric tolerances.  

CHK fonts are installed as part of installation directory. Total 40 MB. All other (Indian / International) font files are embedded as binary resource in the .exe file itself. Non-Compressed they are mere ~12 MB. More design principles / analysis is mentioned in the following code file.  

{{< codefile src="code-core/UserInterface-TextTranslations.h" >}}

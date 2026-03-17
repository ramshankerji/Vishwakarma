// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

enum class UILanguage : uint8_t
{
    English = 0,

    // 22 Indian scheduled languages
    Hindi,
    Bengali,
    Telugu,
    Marathi,
    Tamil,
    Urdu,
    Gujarati,
    Kannada,
    Malayalam,
    Odia,
    Punjabi,
    Assamese,
    Maithili,
    Santali,
    Kashmiri,
    Nepali,
    Sindhi,
    Konkani,
    Manipuri,
    Bodo,
    Dogri,
    Sanskrit,

    // Major global engineering languages
    ChineseSimplified,
    ChineseTraditional,
    Japanese, // Covers all of Katakana , Kanji and Hiragana symbols within same fonts.
    Korean,
    German,
    French,
    Spanish,
    Portuguese,
    Russian,
    Italian,
    Turkish,
    Polish,
    Dutch,
    Swedish,
    Czech,
    Hungarian,
    Ukrainian,
    Vietnamese,
    Thai,
    Indonesian,
    Arabic,
    Persian, // Farsi - Iran engineering market
    Filipino,// Tagalog

    COUNT
};

/*
ChatGPT analysis of population coverage by above 46 languages:

| Metric                         | Result     |
| ------------------------------ | ---------- |
| World population coverage      | **90–94%** |
| Engineering workforce coverage | **97–99%** |
| India coverage                 | **~99%**   |
| Europe coverage                | **~95%**   |
| Americas coverage              | **~95%**   |

All these 46 languages translate to 13 unique scripts. Unicode handles all of the well.

| Script        | Languages                             |
| ------------- | ------------------------------------- |
| Latin         | English, German, French, Spanish, etc |
| Devanagari    | Hindi, Marathi, Nepali etc            |
| Bengali       | Bengali, Assamese                     |
| Gurmukhi      | Punjabi                               |
| Gujarati      | Gujarati                              |
| Odia          | Odia                                  |
| Tamil         | Tamil                                 |
| Telugu        | Telugu                                |
| Kannada       | Kannada                               |
| Malayalam     | Malayalam                             |
| Arabic script | Urdu, Arabic, Persian, Kashmiri       |
| Chinese Han   | Chinese + Japanese Kanji              |
| Japanese kana | Hiragana/Katakana                     |
| Hangul        | Korean                                |
| Thai          | Thai                                  |

Professional CAD software language coverage (As per ChatGPT).
| Software   | Languages |
| ---------- | --------- |
| AutoCAD    | ~15       |
| SolidWorks | ~13       |
| Fusion360  | ~10       |
| CATIA      | ~8        |
All softwares listed below are copy right of respective software companies.

HENCE OUR LANGUAGE LIST IS FROZEN ! ;)

Estimated size overhead of bundling all the fonts:

| Font                         | Typical Size |
| ---------------------------- | ------------ |
| Noto Sans (Latin + extended) | ~2 MB        |
| Noto Sans Devanagari         | ~1.5 MB      |
| Noto Sans Bengali            | ~1.3 MB      |
| Noto Sans Gurmukhi           | ~0.9 MB      |
| Noto Sans Gujarati           | ~1.0 MB      |
| Noto Sans Oriya (Odia)       | ~1.1 MB      |
| Noto Sans Tamil              | ~0.9 MB      |
| Noto Sans Telugu             | ~1.2 MB      |
| Noto Sans Kannada            | ~1.2 MB      |
| Noto Sans Malayalam          | ~1.4 MB      |
| Noto Sans Arabic             | ~1.2 MB      |
| Noto Sans Thai               | ~0.7 MB      |

Subtotal (non-CJK): ≈ 14–15 MB

| Font                                   | Approx Size |
| -------------------------------------- | ----------- |
| Noto Sans CJK SC (Simplified Chinese)  | ~16–18 MB   |
| Noto Sans CJK TC (Traditional Chinese) | ~16–18 MB   |
| Noto Sans CJK JP (Japanese)            | ~16–18 MB   |
| Noto Sans CJK KR (Korean)              | ~16–18 MB   |

CJK 3 variants (SC + JP + KR): ≈ 48–54 MB

Total: ≈ 65 MB , ~60% Compression expected in Installer. ≈ 40 MB. Acceptable.

Runtime: Entire font files will not be loaded at runtime.
They will be loaded on demand to minimize memory footprint.

*/
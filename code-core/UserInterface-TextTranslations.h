// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

enum class UILanguage : uint8_t
{
    English = 0,

    // 22 Indian scheduled languages
    /* Here is the list of the given languages arranged in descending order of the number of speakers.
    2011 Census of India data for total speakers, including both native/mother tongue and second-language speakers where reported,
    as this is the most comprehensive official source available). */

    Hindi, // ~528–691 million total speakers; ~43.63% of India's population as native speakers alone)
    Bengali, // ~97–107 million
    Marathi, // ~83–99 million
    Telugu, // ~81–94 million
    Tamil, // ~69–76 million
    Gujarati, // ~55–60 million
    Urdu, // ~50–63 million
    Kannada, // ~43–58 million
    Odia, // ~37–42 million
    Malayalam, // ~34–35 million
    Punjabi, // ~33–36 million
    Assamese, // ~15–23 million
    Maithili, // ~13–14 million, based on ~1.12% share)
    Santali, // ~7.3–7.7 million
    Kashmiri, // ~6.8–7 million
    Nepali, // ~2.9–3 million
    Sindhi, // ~2.7–3 million
    Dogri, // ~2.6–2.8 million
    Konkani, // ~2.2–2.6 million
    Manipuri, // (Meitei) ~1.7–2 million
    Bodo, // ~1.4–1.6 million
    Sanskrit, // ~25,000 native speakers; higher if including those reporting knowledge, but still by far the smallest)

    // Major global engineering languages. Population number by Grok citing 
    ChineseSimplified, // Both Chinese combined ~1.18–1.20 billion total speakers (mostly native)
    ChineseTraditional, //(Mandarin Chinese)
    Spanish, // ~558–560 million
    Portuguese, // ~264–270 million
    Russian, // ~253–260 million
    French, // ~312–330 million (some sources place it slightly above or near Arabic depending on L2 counting)
    Arabic, // ~335 million (Modern Standard Arabic + varieties; widely used in engineering contexts across the Middle East)
    Indonesian, // ~200–255 million
    German, // ~130–134 million
    Japanese, // ~125–126 million. Covers all of Katakana , Kanji and Hiragana symbols within same fonts.
    Vietnamese, // ~85–97 million
    Turkish, // ~80–90 million
    Persian, // (Farsi) — ~70–82 million. Farsi - Iran engineering market
    Korean, // ~80–85 million
    Italian, // ~65–90 million
    Thai, // ~60–70 million
    Polish, // ~45–50 million
    Ukrainian, // ~35–45 million
    Dutch, // ~25–30 million
    Filipino, // (Tagalog) ~80–90 million total (native ~25–30 million + significant L2 in Philippines)
    Swedish, // ~10–15 million
    Czech, // ~10–12 million
    Hungarian, // ~12–14 million

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

All these 46 languages translate to 13 unique scripts. Unicode handles all of them well.

| Script        | Languages                             |
| ------------- | ------------------------------------- |
| Latin         | English, German, French, Spanish, etc |
| Cyrillic      | Russian, Ukrainian, Bulgarian, etc    |
| Devanagari    | Hindi, Marathi, Nepali, Sanskrit etc  |
| Bengali       | Bengali, Assamese                     |
| Gurmukhi      | Punjabi                               |
| Gujarati      | Gujarati                              |
| Odia          | Odia                                  |
| Tamil         | Tamil                                 |
| Telugu        | Telugu                                |
| Kannada       | Kannada                               |
| Malayalam     | Malayalam                             |
| Meetei Mayek  | Manipuri (Meitei)                     |
| Ol Chiki      | Santali                               |
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
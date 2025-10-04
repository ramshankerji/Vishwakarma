---
title: "Coding Conventions"
weight: 100104
---
The majority of our contributors are expected to be domain (civil/mechanical/electrical/instrumentation/chemical and so on) engineers, in addition to computer science graduates.
Hence we enforce minimal guideline. No pull request shall be rejected because some bizarre coding convention listed below is not followed. Here are some of our basic coding conventions.

1. Variables are camelCase, i.e. start with small alphabet, subsequent words in same variable stick together and start with capital letters.
2. Function and Class Names are "CapitalSmallCapitalSmall" cased. i.e. All words in name start with Capital letters.
3. Constants defined are CAPITAL_LETTER_WITH_UNDERSCORE type.
4. .c / .cpp / .h / .py files should be approximately 100 to 120 characters wide.
5. Very few already defined top-level / global variable / classes are defined using unicode (hindi) character. All other variables / class names etc must be english alphanumerical.
6. Loop counter are preferred to be single alphabet say i, j, m, n etc. Or can be a logical name also.
7. g_ or m_ or any other type/behavior hinting prefixes having "_" in variable names must not be present.
8. Do not split a code file until it reaches 1000+ lines of code. Must split if it reaches 5000. We want 1 file to be readable by 1 human in 1 evening coding session.
9. If a function has been used only once, inline it immediately. Do not create a common function until you have at least >3 uses of same function.

That's it. These are approximate guidelines, and not to be treated as show stopper for contributors.

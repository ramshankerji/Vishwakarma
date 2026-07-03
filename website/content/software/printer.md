---
title: "Printer Interfaces"
weight: 100208
---
All source codes are in PrinterController.cpp / PrinterController.h

* Color policy For CAD printing:
Default background: white, 
Default line output: black, 
Optional: monochrome mode, 
Optional: color plot style table, 
Render target: BGRA8 sRGB, 
Do not use HDR for print MVP

HDR is not useful for normal CAD printing. Let the printer driver handle device color conversion.

* Print preview, Use the same pipeline at lower DPI:
Preview DPI: 96 or 150,
Print DPI: 300 or 600

Never maintain a separate preview renderer. Same layout, same line weight policy, same page transfor

* Text policy For MVP:
Use current MSDF text atlas.
Render text as high-res raster.

At 600 DPI it will look good.

* Modern Windows printing options
1. D3D12 high-resolution raster → GDI printer DC. Best MVP. Pipeline:  
CAD model → D3D12 offscreen render target at print DPI → GPU readback → BGRA bitmap → GDI StartDoc/StartPage/StretchDIBits → printer  
Pros: simple, works with your renderer, identical visual output, no second vector renderer required.  
Cons: output is raster, so extremely large pages need tiling.  

Use this first.

2. D3D12 high-resolution raster → XPS / Print Document Package image page. Good second option. Pipeline:  
CAD model → D3D12 bitmap tiles/page image → package as fixed page image → Print Document Package API  
Windows desktop apps can use several print APIs, including the Print Document Package API, Print Spooler API, Print Ticket API, and GDI Print API. Microsoft lists Print Document Package API as available for Windows 8 and later desktop printing.  

Pros: more modern than raw GDI.  
Cons: more COM/XPS plumbing.  

* Typical network printers support two different layers:
1. Network transport / discovery: IPP, WSD, Bonjour/mDNS, SNMP, HTTP admin page, TCP/IP, sometimes RAW 9100 and LPR
2. Page description / print language: Raster image, PWG Raster, Apple raster/URF, PDF, PCL, PostScript, XPS, vendor-private raster

For our CAD application, the safest assumption is: Application → Windows Print Spooler → Installed printer driver/class driver → printer  
Do not assume the printer accepts PCL, PostScript, PDF, or your own bitmap stream directly.  


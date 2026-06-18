---
title: "2D Drawings"
weight: 20000
layout: "section"
---
Here we deal with 2D Drawings. 2D drawings have traditionally been just a digital version of a sheet of paper.

Subsequently many disciplines started developing certain conventions to encode domain specific information through their own exclusive practices.

We intend to develop the following minimum feature set in our application for 2D applications.

### Basic 2D Objects & Creation
- Lines, Polylines, and Polygons (Triangles, Rectangles, Pentagons, Hexagons, etc.)
- Circles, Arcs, and Ellipses
- NURBS (Non-Uniform Rational B-Splines) for advanced curves
- Text and Special Text
- Dimensions, Leaders, and Radii
- Clouds, Hatches, and Points
- Arrays and 2D Grids
- 2D Blocks

### Modification Commands
- Copy, Mirror, Offset
- Move, Rotate, Stretch, Scale
- Extend, Trim
- Fillet, Chamfer
- Explode

### Intelligent 2D (Advanced)
Intelligent 2D symbols with embedded parameters that users can place in 2D views:
- Vertical and Horizontal Vessels (VVessel2D, HVessel2D)
- Heat Exchangers (Exchanger2D)
- Filters (Filter2D)
- Pumps (Pump2D)
- Air Coolers (Aircooler2D)
- Compressors (Compressor2D)
- Pipes (Pipe2D)
- Instruments (Instrument2D)
- **Parameter Editing**: Direct editing of the embedded parameters for all intelligent symbols.

/*
This file represents our 2D CAD Modules.
*/


/*
To draw a line from (𝑥1,𝑦1) (x1, y1) to (𝑥2,𝑦2)(x2, y2) using the 
SetPixel function in C++, you can use Bresenham's Line Algorithm. 
This algorithm is efficient and works well with integer arithmetic,
making it suitable for drawing lines on raster displays.
*/
void Draw2DLine(HDC hdc, int x1, int y1, int x2, int y2) {
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        SetPixel(hdc, x1, y1, RGB(0, 0, 0)); // Set the pixel at (x1, y1)

        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}
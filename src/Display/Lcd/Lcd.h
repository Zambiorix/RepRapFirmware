#ifndef SRC_DISPLAY_LCD_LCD_H
#define SRC_DISPLAY_LCD_LCD_H

#include "RepRapFirmware.h"

#if SUPPORT_DIRECT_LCD

#include <Print.h>
#include "Fonts/LcdFont.h"
#include <Hardware/SharedSpi/SharedSpiClient.h>
#include <General/SafeVsnprintf.h>

typedef uint16_t PixelNumber;

// Class for driving graphical LCD

// Derive the LCD class from the Print class so that we can print stuff to it in alpha mode
class Lcd
{
public:
	// Construct a GLCD driver.
	Lcd(PixelNumber nr, PixelNumber nc, const LcdFont * const fnts[], size_t nFonts) noexcept;
	virtual ~Lcd();

	// Flush just some data, returning true if this needs to be called again
	virtual bool FlushSome() noexcept = 0;

	// Get the display type
	virtual const char *GetDisplayTypeName() const noexcept = 0;

	// Get the SPI frequency
	virtual uint32_t GetSpiFrequency() const noexcept = 0;

	// Initialize the display
	virtual void Init(Pin p_csPin, Pin p_a0Pin, bool csPolarity, uint32_t freq, uint8_t p_contrastRatio, uint8_t p_resistorRatio) noexcept = 0;

	// Clear part of the display and select non-inverted text.
	virtual void Clear(PixelNumber top, PixelNumber left, PixelNumber bottom, PixelNumber right) noexcept = 0;

	// Set, clear or invert a pixel
	//  x = x-coordinate of the pixel, measured from left hand edge of the display
	//  y = y-coordinate of the pixel, measured down from the top of the display
	//  mode = whether we want to set or clear the pixel
	virtual void SetPixel(PixelNumber y, PixelNumber x, bool mode) noexcept = 0;

	// Draw a bitmap
	//  x0 = x-coordinate of the top left, measured from left hand edge of the display. Currently, must be a multiple of 8.
	//  y0 = y-coordinate of the top left, measured down from the top of the display
	//  width = width of bitmap in pixels. Currently, must be a multiple of 8.
	//  rows = height of bitmap in pixels
	//  data = bitmap image, must be ((width/8) * rows) bytes long
	virtual void BitmapImage(PixelNumber top, PixelNumber left, PixelNumber height, PixelNumber width, const uint8_t data[]) noexcept = 0;

	// Draw a bitmap row
	//  x0 = x-coordinate of the top left, measured from left hand edge of the display
	//  y0 = y-coordinate of the top left, measured down from the top of the display
	//  width = width of bitmap in pixels
	//  data = bitmap image, must be ((width + 7)/8) bytes long
	virtual void BitmapRow(PixelNumber top, PixelNumber left, PixelNumber width, const uint8_t data[], bool invert) noexcept = 0;

	// Draw a line.
	//  x0 = x-coordinate of one end of the line, measured from left hand edge of the display
	//  y0 = y-coordinate of one end of the line, measured down from the top of the display
	//  x1, y1 = coordinates of the other end od the line
	//  mode = whether we want to set or clear each pixel
	void Line(PixelNumber top, PixelNumber left, PixelNumber bottom, PixelNumber right, bool mode) noexcept;

	// Draw a circle
	//  x0 = x-coordinate of the centre, measured from left hand edge of the display
	//  y0 = y-coordinate of the centre, measured down from the top of the display
	//  radius = radius of the circle in pixels
	//  mode = whether we want to set or clear each pixel
	void Circle(PixelNumber p_row, PixelNumber col, PixelNumber radius, bool mode) noexcept;

	// Select the font to use for subsequent calls to write() in graphics mode
	void SetFont(size_t newFont) noexcept;

	PixelNumber GetNumRows() const noexcept { return numRows; }
	PixelNumber GetNumCols() const noexcept { return numCols; }

	// Write a single character in the current font. Called by the 'print' functions.
	//  c = character to write
	// Returns the number of characters written (1 if we wrote it, 0 otherwise)
	size_t write(uint8_t c) noexcept;

	// Write a space
	void WriteSpaces(PixelNumber numPixels) noexcept;

	// Return the number of fonts
	size_t GetNumFonts() const noexcept { return numFonts; }

	// Get the current font height
	PixelNumber GetFontHeight() const noexcept;

	// Get the height of a specified font
	PixelNumber GetFontHeight(size_t fontNumber) const noexcept;

	// Select normal or inverted text (only works in graphics mode)
	void TextInvert(bool b) noexcept;

	// Clear the whole display and select non-inverted text.
	void ClearAll() noexcept
	{
		Clear(0, 0, numRows, numCols);
	}

	// Set the cursor position
	//  r = row, the number of pixels from the top of the display to the top of the character.
	//  c = column, is the number of pixels from the left hand edge of the display and the left hand edge of the character.
	void SetCursor(PixelNumber r, PixelNumber c) noexcept;

	// Get the cursor row. Useful after we have written some text.
	PixelNumber GetRow() const noexcept { return row; }

	// Get the cursor column. Useful after we have written some text.
	PixelNumber GetColumn() const noexcept { return column; }

	// Set the left margin. This is where the cursor goes to when we print newline.
	void SetLeftMargin(PixelNumber c) noexcept;

	// Set the right margin. In graphics mode, anything written will be truncated at the right margin. Defaults to the right hand edge of the display.
	void SetRightMargin(PixelNumber r) noexcept;

	// Clear a rectangle from the current position to the right margin (graphics mode only). The height of the rectangle is the height of the current font.
	void ClearToMargin() noexcept;

	// Flush the display buffer to the display. Data will not be committed to the display until this is called.
	void FlushAll() noexcept;

	// printf to LCD
	int printf(const char* fmt, ...) noexcept;

protected:
	// Write one column of character data at (row, column)
	virtual void WriteColumnData(uint16_t columData, uint8_t ySize) noexcept = 0;

	// Write a decoded character
	size_t writeNative(uint16_t c) noexcept;

	const PixelNumber numRows, numCols;
	PixelNumber row, column;
	PixelNumber leftMargin, rightMargin;

	const LcdFont * const *fonts;
	const size_t numFonts;
	size_t currentFontNumber;						// index of the current font
	bool textInverted;

private:
	uint32_t charVal;
	uint16_t lastCharColData;						// data for the last non-space column, used for kerning
	uint8_t numContinuationBytesLeft;
	bool justSetCursor;
};

#endif

#endif	// SRC_DISPLAY_LCD_LCD_H

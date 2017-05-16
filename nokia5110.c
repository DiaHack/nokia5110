// Nokia 5110 using the SPI interface
// Written by Larry Bank (bitbank@pobox.com)
// Project started 4/8/2017
//
// The Nokia 5110 LCD display is controlled through the SPI interface
// and two GPIO pins to control the RST (reset), and D/C (data/command)
// control lines. 
// The LCD controller is set to "horizontal mode". This divides the display
// into 6 84x8 "pages" or strips. Each data write advances the output
// automatically to the next address. The bytes are arranged such that the LSB
// is the topmost pixel and the MSB is the bottom.
// The font data comes from another source and must be rotated 90 degrees
// (at init time) to match the orientation of the bits on the display memory.
// A copy of the display memory is maintained by this code so that single pixel
// writes can occur without having to read from the display controller.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>
#include "nokia5110.h"

extern unsigned char ucFont[];
static int iScreenOffset; // current write offset of screen data
static unsigned char ucScreen[504]; // local copy of the image buffer
static int file_spi = 0;
static int iDCPin, iResetPin, iLEDPin;

static void nokiaWriteCommand(unsigned char);
int nokiaFill(unsigned char);
static void RotateFont90(void);

//
// Lookup table to translate header pin numbers into wiringPi GPIO numbers
// invalid/unusable pins return 0xff
//
static unsigned char ucWPPins[] = {0xff,0xff,0xff,8,0xff,9,0xff,7,15,0xff,16,0,1,
        2,0xff,3,4,0xff,5,12,0xff,13,6,14,10,0xff,11,30,31,21,0xff,22,26,23,0xff,24,27,25,28,0xff,29};
typedef enum
{
 MODE_DATA = 0,
 MODE_COMMAND
} DC_MODE;

// Sets the D/C pin to data or command mode
void nokiaSetMode(int iMode)
{
	digitalWrite(iDCPin, (iMode == MODE_DATA));
} /* nokiaSetMode() */

// Opens a file system handle to the SPI device
// Initializes the LCD controller into "horizontal write mode"
// Prepares the font data for the orientation of the display
// Parameters: GPIO pin numbers used for the DC/RST/LED control lines
//
int nokiaInit(int iChannel, int iDC, int iReset, int iLED)
{

        iDCPin = ucWPPins[iDC];
        iResetPin = ucWPPins[iReset];
        iLEDPin = ucWPPins[iLED];
        if (iDCPin == 0xff || iResetPin == 0xff || iLEDPin == 0xff) // problem
        {
                printf("One or more invalid GPIO pin numbers\n");
                return -1;
        }

        wiringPiSetup(); // initialize GPIO interface
	file_spi = wiringPiSPISetup(iChannel, 4000000); // Initialize SPI channel at 4Mhz
	if (file_spi == -1)
	{
		fprintf(stderr, "Failed to open the SPI bus\n");
		file_spi = 0;
		return -1;
	}

        pinMode(iDCPin, OUTPUT);
        pinMode(iResetPin, OUTPUT);
	pinMode(iLEDPin, OUTPUT);

	// Start by reseting the LCD controller
        digitalWrite(iResetPin, HIGH);
	delay(50);
	digitalWrite(iResetPin, LOW);
	delay(5);
	digitalWrite(iResetPin, HIGH); // take it out of reset
	nokiaBacklight(1); // turn on the backlight

	nokiaSetMode(MODE_COMMAND);
	nokiaWriteCommand(0x21); // set advanced commands
	nokiaWriteCommand(0xa4); // set LCD Vop (contrast)
	nokiaWriteCommand(0x04); // set temperature coefficient
	nokiaWriteCommand(0x14); // set LCD bias mode 1:40
	nokiaWriteCommand(0x20); // set simple command mode
	nokiaWriteCommand(0x0c); // set display mode to normal

	nokiaFill(0); // erase memory
	RotateFont90(); // fix font orientation for OLED
	return 0;

} /* nokiaInit() */

// Controls the LED backlight
void nokiaBacklight(int bOn)
{
	digitalWrite(iLEDPin, (bOn) ? LOW:HIGH);
} /* nokiaBacklight() */

// Sends a command to turn off the LCD display
// Closes the SPI file handle
void nokiaShutdown()
{
	if (file_spi != 0)
	{
		nokiaSetMode(MODE_COMMAND);
		nokiaBacklight(0); // turn off the backlight
		nokiaWriteCommand(0x24); // power down
		close(file_spi);
		file_spi = 0;
	}
}

void nokiaWriteCommand(unsigned char c)
{
unsigned char buf[2];

	nokiaSetMode(MODE_COMMAND);
	buf[0] = c;
	write(file_spi, buf, 1);
} /* nokiaWriteCommand() */

int nokiaSetContrast(unsigned char ucContrast)
{
        if (file_spi == 0)
                return -1;

	nokiaWriteCommand(0x21); // set advanced command mode
	nokiaWriteCommand(0x80 | ucContrast); // set contrast
	nokiaWriteCommand(0x20); // set simple command mode
	return 0;
} /* nokiaSetContrast() */

// Send commands to position the "cursor" to the given
// row and column
static void nokiaSetPosition(int x, int y)
{
	nokiaWriteCommand(0x40 | y); // set Y
	nokiaWriteCommand(0x80 | x); // set X
	iScreenOffset = (y*84)+x;
}

// Write a block of pixel data to the LCD
// Length can be anything from 1 to 504 (whole display)
static void nokiaWriteDataBlock(unsigned char *ucBuf, int iLen)
{

	nokiaSetMode(MODE_DATA);
	write(file_spi, ucBuf, iLen);
	// Keep a copy in local buffer
	memcpy(&ucScreen[iScreenOffset], ucBuf, iLen);
	iScreenOffset += iLen;
}

// Set (or clear) an individual pixel
// The local copy of the frame buffer is used to avoid
// reading data from the display controller
int nokiaSetPixel(int x, int y, unsigned char ucColor)
{
int i;
unsigned char uc, ucOld;

	if (file_spi == 0)
		return -1;

	i = ((y >> 3) * 84) + x;
	if (i < 0 || i > 503) // off the screen
		return -1;
	uc = ucOld = ucScreen[i];
	uc &= ~(0x1 << (y & 7));
	if (ucColor)
	{
		uc |= (0x1 << (y & 7));
	}
	if (uc != ucOld) // pixel changed
	{
		nokiaSetPosition(x, y>>3);
		nokiaWriteDataBlock(&uc, 1);
	}
	return 0;
} /* nokiaSetPixel() */

//
// Read the pixel at the given x,y
// if the library is not initialized, or the coordinates are
// not within the valid range, it returns 0
//
int nokiaGetPixel(int x, int y)
{
int i;

   if (file_spi == 0)
	return 0; // invalid request returns "black" pixel
   i = ((y >> 3) * 84) + x;
   if (i < 0 || i > 503) // off the screen
	return 0;
   return (ucScreen[i] & (1<< (y & 7)));
} /* nokiaGetPixel() */

// Draw a string of small (8x8) or large (16x24) characters
// At the given col+row
int nokiaWriteString(int x, int y, char *szMsg, int bLarge)
{
int i, iLen;
unsigned char *s;

	if (file_spi == 0) return -1; // not initialized

	iLen = strlen(szMsg);
	if (bLarge) // draw 16x32 font
	{
		if (iLen+x > 5) iLen = 5-x;
		if (iLen < 0) return -1;
		for (i=0; i<iLen; i++)
		{
			s = &ucFont[9728 + (unsigned char)szMsg[i]*64];
			nokiaSetPosition(x+(i*16), y);
			nokiaWriteDataBlock(s, 16);
			nokiaSetPosition(x+(i*16), y+1);
			nokiaWriteDataBlock(s+16, 16);	
			nokiaSetPosition(x+(i*16), y+2);
			nokiaWriteDataBlock(s+32, 16);	
		}
	}
	else // draw 8x8 font
	{
		nokiaSetPosition(x, y);
		if ((8*iLen) + x > 84) iLen = (84 - x)/8; // can't display it all
		if (iLen < 0)return -1;

		for (i=0; i<iLen; i++)
		{
			s = &ucFont[(unsigned char)szMsg[i] * 8];
			nokiaWriteDataBlock(s, 8); // write character pattern
		}	
	}
	return 0;
} /* nokiaWriteString() */

// Fill the frame buffer with a byte pattern
// e.g. all off (0x00) or all on (0xff)
int nokiaFill(unsigned char ucData)
{
int y;
unsigned char temp[84];

	if (file_spi == 0) return -1; // not initialized

	memset(temp, ucData, 84);
	for (y=0; y<6; y++)
	{
		nokiaSetPosition(0,y); // set to (0,Y)
		nokiaWriteDataBlock(temp, 84); // fill with data byte
	} // for y
	return 0;
} /* nokiaFill() */

// Fix the orientation of the font image data
static void RotateFont90(void)
{
unsigned char ucTemp[64];
int i, j, x, y;
unsigned char c, c2, ucMask, *s, *d;

	// Rotate the 8x8 font
	for (i=0; i<256; i++) // fix 8x8 font by rotating it 90 deg clockwise
	{
		s = &ucFont[i*8];
		ucMask = 0x1;
		for (y=0; y<8; y++)
		{
			c = 0;
			for (x=0; x<8; x++)
			{
				c >>= 1;
				if (s[x] & ucMask) c |= 0x80;
			}
			ucMask <<= 1;
			ucTemp[7-y] = c;
		}
		memcpy(s, ucTemp, 8);
	}
	// Rotate the 16x32 font
	for (i=0; i<128; i++) // only 128 characters
	{
		for (j=0; j<4; j++)
		{
			s = &ucFont[9728 + 12 + (i*64) + (j*16)];
			d = &ucTemp[j*16];
			ucMask = 0x1;
			for (y=0; y<8; y++)
			{
				c = c2 = 0;
				for (x=0; x<8; x++)
				{
					c >>= 1;
					c2 >>= 1;
					if (s[(x*2)] & ucMask) c |= 0x80;
					if (s[(x*2)+1] & ucMask) c2 |= 0x80;
				}
				ucMask <<= 1;
				d[7-y] = c;
				d[15-y] = c2;
			} // for y
		} // for j
		memcpy(&ucFont[9728 + (i*64)], ucTemp, 64);
	} // for i
} /* RotateFont90() */

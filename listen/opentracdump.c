/* OpenTRAC packet decoding by Scott Miller, N1VG
   Copyright (c) 2003 Scott Miller
   Protocol v1.0 draft, Modified 19 Jun 2003
*/

#include <stdio.h>
#include <time.h>
#include <string.h>
#include "listen.h"

#define MAX_UNIT_INDEX 28

const char *units[]={"Volts","Amperes","Watts","Kelvins","Meters","Seconds",
		"Meters/Second","Liters","Kilograms","Bits/Second",
		"Bytes","Radians","Radians/Second","Square Meters",
		"Joules","Newtons","Pascals","Hertz","Meters/Sec^2",
		"Grays","Lumens","Cubic Meters/Second","Pascal Seconds",
		"Kilograms/Meter^3","Radians/Second^2","Coulombs",
		"Farads","Siemens","Count"};
		
// Return values: 0 = OK, -1 = Couldn't Decode, -2 = Invalid Data

int decode_sequence(unsigned char *element, int element_len);
int decode_origination(unsigned char *element, int element_len);
int decode_entityid(unsigned char *element, int element_len);
int decode_position(unsigned char *element, int element_len);
int decode_timestamp(unsigned char *element, int element_len);
int decode_comment(unsigned char *element, int element_len);
int decode_courseandspeed(unsigned char *element, int element_len);
int decode_ambiguity(unsigned char *element, int element_len);
int decode_country(unsigned char *element, int element_len);
int decode_displayname(unsigned char *element, int element_len);
int decode_waypoint(unsigned char *element, int element_len);
int decode_symbol(unsigned char *element, int element_len);
int decode_pathtrace(unsigned char *element, int element_len);
int decode_heardby(unsigned char *element, int element_len);
int decode_availablenets(unsigned char *element, int element_len);
int decode_maidenhead(unsigned char *element, int element_len);
int decode_gpsquality(unsigned char *element, int element_len);
int decode_acreg(unsigned char *element, int element_len);
int decode_rivergauge(unsigned char *element, int element_len);
int decode_hazmat(unsigned char *element, int element_len);
int flag_emergency(void);
int flag_attention(void);

int extract_ssid(unsigned char *call);

int decode_units(unsigned int unitnum, unsigned char *element, int element_len);

unsigned char origin_call[7]; // Who's talking
unsigned char origin_ssid;
unsigned char entity_call[7]; // What they're talking about
unsigned char entity_ssid;
unsigned int  entity_serial;
unsigned int  entity_sequence;

/* Dump an OpenTRAC packet */
void opentrac_dump(unsigned char *data, int length, int hexdump)
{
	int elen;
	int etype;
	int decoded = 0;
	
	lprintf(T_PROTOCOL, "OpenTRAC decode (%d bytes):\r\n", length);
	strcpy(origin_call, "SENDER"); // Listen doesn't tell us the sender
	origin_ssid = 0;
	entity_serial = 0;
	entity_sequence = 0;
	while (decoded < length) {
		elen = (int)*data;
		decoded += (elen & 0x7f)+1;
		if (elen & 0x80) { // See if it's got a 16-bit ID
			elen = (elen & 0x7f) - 2; // Strip the extid flag
			etype = get16(++data);
		}
		else {
			elen--; // Don't count the type byte
			etype = (int)*(data+1);
		}
		data+=2; // Skip to the body
		lprintf(T_OPENTRAC, "EID 0x%0x len %d: ", etype, elen);
		switch (etype) {
			case (0x00): // Sequence
				decode_sequence(data, elen);
				break;
			case (0x01): // Originating Station
				decode_origination(data, elen);
				break;
			case (0x02): // Entity ID
				decode_entityid(data, elen);
				break;
			case (0x10): // Position report
				decode_position(data, elen);
				break;
			case (0x11): // Timestamp
				decode_timestamp(data, elen);
				break;
			case (0x12): // Comment
				decode_comment(data, elen);
				break;
			case (0x13): // Course and Speed
				decode_courseandspeed(data, elen);
				break;
			case (0x14): // Positional Ambiguity
				decode_ambiguity(data, elen);
				break;
			case (0x15): // Country Code
				decode_country(data, elen);
				break;
			case (0x16): // Display Name
				decode_displayname(data, elen);
				break;
			case (0x17): // Waypoint Name
				decode_waypoint(data, elen);
				break;
			case (0x18): // Map Symbol
				decode_symbol(data, elen);
				break;
			case (0x20): // Path Trace
				decode_pathtrace(data, elen);
				break;
			case (0x21): // Heard-By List
				decode_heardby(data, elen);
				break;
			case (0x22): // Available Networks
				decode_availablenets(data, elen);
				break;
			case (0x32): // Maidenhead Locator
				decode_maidenhead(data, elen);
				break;
			case (0x34): // GPS Data Quality
				decode_gpsquality(data, elen);
				break;
			case (0x35): // Aircraft Registration
				decode_acreg(data, elen);
				break;
			case (0x42): // River Flow Gauge
				decode_rivergauge(data, elen);
				break;
			case (0x100): // Emergency/distress flag
				flag_emergency();
				break;
			case (0x101): // Attention/ident flag
				flag_attention();
				break;
			case (0x300): // Hazmat
				decode_hazmat(data, elen);
				break;
			default: // Everything else
				if ((etype & 0xff00) == 0x500) {
					decode_units(etype & 0x00ff, data, elen);
				}
				else {
					lprintf(T_OPENTRAC, "Unknown Element Type\r\n");
				}
		}
		data+=elen;
	}
}

int decode_sequence(unsigned char *element, int element_len) {
	// 0x00 Sequence number - 16 bit integer
	if (element_len != 2 && element_len != 0) return -1;

	if (!element_len) {
		entity_sequence++;
	}
	else {
		entity_sequence = get16(element);
	}

	lprintf(T_OPENTRAC,"Sequence: %d\r\n",entity_sequence);

	return 0;
}

int decode_origination(unsigned char *element, int element_len) {
	// 0x01 Originating Station - Callsign, SSID, Sequence, and Network
	unsigned char network;

	if (element_len < 8) return -1;
	if (element_len > 9) return -1;

	memcpy(origin_call, element, 6);
	origin_call[6]=0;
	origin_ssid = extract_ssid(origin_call);
	entity_sequence = get16(element+6);
	strcpy(entity_call, origin_call);
	entity_ssid = origin_ssid;
	entity_serial = 0;
	if (element_len == 9) {
		network = *(element+9);
		lprintf(T_OPENTRAC, "Origin: %s-%d seq %d net %d\r\n",origin_call,origin_ssid,entity_sequence,network);
	}
	else lprintf(T_OPENTRAC, "Origin: %s-%d seq %d direct\r\n",origin_call,origin_ssid,entity_sequence);

	return 0;
}

int decode_entityid(unsigned char *element, int element_len) {
	// 0x02 Entity ID

   if (element_len > 10) return -1;

	if (element_len > 5) {
   		memcpy(entity_call, element, 6);
		entity_call[6]=0;
		entity_ssid = extract_ssid(entity_call);
	}
	else {
		strcpy(entity_call, origin_call);
	}

	switch (element_len) {
		case 0:
			entity_serial++;
			entity_sequence = 0;
			break;
		case 2:
			entity_serial = get16(element);
			entity_sequence = 0;
			break;
		case 4:
			entity_serial = get16(element);
			entity_sequence = get16(element+2);
			break;
		case 6:
			entity_serial = 0;
			break;
		case 8:
			entity_serial = get16(element+6);
			entity_sequence = 0;
			break;
		case 10:
			entity_serial = get16(element+6);
			entity_sequence = get16(element+8);
			break;
		default:
			return -1;
	}

	lprintf(T_OPENTRAC, "Entity %s-%d:%04x #%d\r\n", entity_call, entity_ssid, entity_serial, entity_sequence);

	return 0;
}

int decode_position(unsigned char *element, int element_len) {
	// 0x10 Position Report - Lat/Lon/<Alt>
	// Lat/Lon is WGS84, 180/2^31 degrees,  Alt is 1/100 meter
	const double semicircles = 11930464.71111111111;
	double lat, lon;
	float alt = 0;

	if (element_len < 8) return -1; // Too short!
	if (element_len > 11) return -1; // Too long!

	lat = get32(element) / semicircles;
	lon = get32(element+4) / semicircles;
	if (element_len == 11) {
		alt = ((((*(element+8))<<16)+get16(element+9))/100)-10000;
	}
	if (lat >= 90 || lat <= -90 || lon >= 180 || lon <= -180) return -2;
	lprintf(T_OPENTRAC, "Position: Lat %f Lon %f Alt %f\r\n",lat,lon,alt);

	return 0;
}

int decode_timestamp(unsigned char *element, int element_len) {
	// 0x11 Timestamp - Unix format time (unsigned)
	long rawtime = 0;

	if (element_len != 4) return -1;

	rawtime = get32(element);
	lprintf(T_OPENTRAC, "Time: %s", ctime(&rawtime));

	return 0;
}

int decode_comment(unsigned char *element, int element_len) {
	// 0x12 Freeform Comment - ASCII text
	char comment[127];

	if (element_len > 126) return -1;  // shouldn't be possible

	strncpy(comment, element, element_len);
	comment[element_len] = 0;
	lprintf(T_OPENTRAC, "Text: %s\r\n", comment);

	return 0;
}

int decode_courseandspeed(unsigned char *element, int element_len) {
	// 0x13 Course and Speed - Course in degrees, speed in 1/50 m/s
	unsigned int course;
	unsigned int rawspeed;
	float speed;

	if (element_len != 3) return -1;

	course = (*element<<1) + ((*(element+1)&0x8000) >> 15);
	rawspeed = (get16(element+1) & 0x7ffff);
	speed = (float)rawspeed*0.072; // kph
	if (course >= 360) return -2;
	lprintf(T_OPENTRAC, "Course: %d Speed: %f kph\r\n", course, speed);

	return 0;
}

int decode_ambiguity(unsigned char *element, int element_len) {
	// 0x14 Positional Ambiguity - 16 bits, in meters
	int ambiguity;

	if (element_len != 2) return -1;

	ambiguity = get16(element);
	lprintf(T_OPENTRAC, "Position +/- %d meters\r\n", ambiguity);
	return 0;
}

int decode_country(unsigned char *element, int element_len) {
	// 0x15 Country Code - ISO 3166-1 and optionally -2
	char country[3];
	char subdivision[4];

	if (element_len < 2) return -1;
	if (element_len > 5) return -1;

	strncpy(country, element, 2);
	country[2] = 0;
	if (element_len > 2) {
		strncpy(subdivision, element+2, element_len-2);
		subdivision[element_len-2] = 0;
		lprintf(T_OPENTRAC, "Country Code %s-%s\r\n", country, subdivision);
	}
	else {
		lprintf(T_OPENTRAC, "Country Code %s\r\n", country);
	}
	return 0;
}

int decode_displayname(unsigned char *element, int element_len) {
// 0x16 - Display Name (UTF-8 text)  
     char displayname[31];        

     if (element_len > 30 || !element_len) return -1;

     strncpy(displayname, element, element_len);
     displayname[element_len] = 0;

     lprintf(T_OPENTRAC, "Display Name: %s\r\n", displayname);
     return 0;
}

int decode_waypoint(unsigned char *element, int element_len) {
// 0x17 - Waypoint Name (up to 6 chars, uppercase)
	char waypoint[7];

	if (element_len > 6 || !element_len) return -1;         

	strncpy(waypoint, element, element_len);
	waypoint[element_len] = 0;

	lprintf(T_OPENTRAC, "Waypoint Name: %s\r\n", waypoint);      
	return 0;
}

int decode_symbol(unsigned char *element, int element_len) {
	// 0x18 Map Symbol - Packed 4-bit integers
	int c;

	if (!element_len) return -1;

	lprintf(T_OPENTRAC, "Symbol: ");
	for (c=0;c<element_len;c++) {
		if (c>0) lprintf(T_OPENTRAC, ".");
		lprintf(T_OPENTRAC, "%d", element[c] >> 4);
		if (element[c] & 0x0f) lprintf(T_OPENTRAC, ".%d", element[c] & 0x0f);
	}
	lprintf(T_OPENTRAC, "\r\n");

	return 0;
}

int decode_pathtrace(unsigned char *element, int element_len) {
	// 0x20 Path Trace - Call/SSID, Network
	char callsign[7];
	int ssid, c, network;

	if (element_len % 7) return -1; // Must be multiple of 7 octets
	if (!element_len) {
		lprintf(T_OPENTRAC, "Empty Path\r\n");
		return 0;
	}

	lprintf(T_OPENTRAC, "Path: ");
	for (c=0; c<element_len; c+=7) {
		memcpy(callsign, element+c, 6);
		ssid = extract_ssid(callsign);
		network = (int)*(element+c+6);
		lprintf(T_OPENTRAC, " %s-%d (%d)", callsign, ssid, network);
	}
	lprintf(T_OPENTRAC, "\r\n");

	return 0;
}

int decode_heardby(unsigned char *element, int element_len) {
	// 0x21 Heard-By List
	int c;

	lprintf(T_OPENTRAC, "Heard By:");
	for (c=0; c<element_len; c++) {
		lprintf(T_OPENTRAC, " %d", (int)*(element+c));
	}
	lprintf(T_OPENTRAC, "\r\n");

	return 0;
}

int decode_availablenets(unsigned char *element, int element_len) {
     // 0x22 Available Networks
     int c;

     lprintf(T_OPENTRAC, "Available Networks:");
     for (c=0; c<element_len; c++) {
          lprintf(T_OPENTRAC, " %d", (int)*(element+c));
     }
     lprintf(T_OPENTRAC, "\r\n");

     return 0;
}

int decode_gpsquality(unsigned char *element, int element_len) {
	// 0x34 GPS Data Quality - Fix, Validity, Sats, PDOP, HDOP, VDOP
	int fixtype, validity, sats;
	const char *fixstr[] = {"Unknown Fix", "No Fix", "2D Fix", "3D Fix"};
	const char *validstr[] = {"Invalid", "Valid SPS", "Valid DGPS",
						 "Valid PPS"};

	if (element_len > 4 || !element_len) return -1;

	fixtype = (element[0] & 0xc0) >> 6;
	validity = (element[0] & 0x30) >> 4;
	sats = (element[0] & 0x0f);
	lprintf(T_OPENTRAC, "GPS: %s %s, %d sats", fixstr[fixtype],
			validstr[validity], sats);
	if (element_len > 1)
		lprintf(T_OPENTRAC, " PDOP=%.1f", (float)element[1]/10);
	if (element_len > 2)
		lprintf(T_OPENTRAC, " HDOP=%.1f", (float)element[2]/10);
	if (element_len > 3)
		lprintf(T_OPENTRAC, " VDOP=%.1f", (float)element[3]/10);
	lprintf(T_OPENTRAC, "\r\n");

	return 0;
}


int decode_acreg(unsigned char *element, int element_len) {
	// 0x35 Aircraft Registration - ASCII text
	char nnumber[9];

	if (element_len > 8) return -1;

	strncpy(nnumber, element, element_len);
	nnumber[element_len]=0;
	lprintf(T_OPENTRAC, "Aircraft ID: %s\r\n", nnumber);
	
	return 0;
}

int decode_rivergauge(unsigned char *element, int element_len) {
	// 0x42 River Flow Gauge - 1/64 m^3/sec, centimeters
	unsigned int flow;
	unsigned int height;
	float flowm;
	float heightm;

	if (element_len != 4) return -1;

	flow = get16(element);
	height = get16(element+2);
	flowm = (float)flow / 64;
	heightm = (float)height / 100;
	lprintf(T_OPENTRAC, "River flow rate: %f Cu M/Sec  Height: %f M\r\n",
			flowm, heightm);

	return 0;
}


int decode_units(unsigned int unitnum, unsigned char *element, int element_len) {
	// 0x0500 to 0x05ff Generic Measurement Elements
	// Values may be 8-bit int, 16-bit int, single float, or double float
	union measurement {
		char c;
		float f;
		double d;
	} *mval;
	int ival; // too much variation in byte order and size for union
	
	if (unitnum > MAX_UNIT_INDEX) return -2; // Invalid unit name
	mval = (void *)element;
	switch (element_len) {
		case 1:
			lprintf(T_OPENTRAC, "%d %s\r\n", mval->c, units[unitnum]);
			break;
		case 2:
			ival = get16(element);
			lprintf(T_OPENTRAC, "%d %s\r\n", ival, units[unitnum]);
			break;
		case 4:
			lprintf(T_OPENTRAC, "%f %s\r\n", mval->f, units[unitnum]);
			break;
		case 8:
			lprintf(T_OPENTRAC, "%f %s\r\n", mval->d, units[unitnum]);
			break;
		default:
			return -1;
	}

	return 0;
}

int flag_emergency(void) {
	// 0x0100 - Emergency / Distress Call
	lprintf(T_ERROR, "* * * EMERGENCY * * *\r\n");
	return 0;
}

int flag_attention(void) {
	// 0x0101 - Attention / Ident
	lprintf(T_PROTOCOL, " - ATTENTION - \r\n");
	return 0;
}

int decode_hazmat(unsigned char *element, int element_len) {
// 0x0300 - HAZMAT (UN ID in lower 14 bits)
	int un_id;

	if (element_len < 2) {
		lprintf(T_OPENTRAC, "HAZMAT: Unknown Material\r\n");
	}
	else
	{
		un_id = get16(element) & 0x3fff;
		lprintf(T_OPENTRAC, "HAZMAT: UN%04d\r\n", un_id);
	}
	return 0;
}

int decode_maidenhead(unsigned char *element, int element_len) {
// 0x32 - Maidenhead Locator (4 or 6 chars)
   char maidenhead[7];

   if (element_len > 6 || !element_len) return -1;

   strncpy(maidenhead, element, element_len);
   maidenhead[element_len] = 0;

   lprintf(T_OPENTRAC, "Grid ID: %s\r\n", maidenhead);
   return 0;
}

int extract_ssid(unsigned char *call) {
	// Strip the SSID from the callsign and return it
	int c, ssid;

   for (c=ssid=0;c<6;c++) {
      ssid |= (call[c] & 0x80) >> (c+2);
      call[c] &= 0x7f;
   }

	return ssid;
}


/*
 * Copyright 1996 Jonathan Naylor G4KLX
 */
#include <stdio.h>
#include <string.h>
#include "listen.h"

#define	ROSE_ADDR_LEN			5

#define	CALL_REQUEST			0x0B
#define	CALL_ACCEPTED			0x0F
#define	CLEAR_REQUEST			0x13
#define	CLEAR_CONFIRMATION		0x17
#define	INTERRUPT			0x23
#define	INTERRUPT_CONFIRMATION		0x27
#define	RESET_REQUEST			0x1B
#define	RESET_CONFIRMATION		0x1F
#define	RESTART_REQUEST			0xFB
#define	RESTART_CONFIRMATION		0xFF
#define	REGISTRATION_REQUEST		0xF3
#define	REGISTRATION_CONFIRMATION	0xF7
#define	DIAGNOSTIC			0xF1
#define	RR				0x01
#define	RNR				0x05
#define	REJ				0x09
#define	DATA				0x00

#define	QBIT				0x80
#define	DBIT				0x40
#define	MBIT				0x10

static char *dump_x25_addr(unsigned char *);
static char *clear_code(unsigned char);
static char *reset_code(unsigned char);
static char *restart_code(unsigned char);

void rose_dump(unsigned char *data, int length, int hexdump)
{
	lprintf(T_ROSEHDR, "X.25: LCI %3.3X : ", (data[0] & 0x0F) + data[1]);

	switch (data[2]) {
	case CALL_REQUEST:
		data   += 4;
		length -= 4;
		lprintf(T_ROSEHDR, "CALL REQUEST - ");
		lprintf(T_ADDR, "%s -> ", dump_x25_addr(data + ROSE_ADDR_LEN));
		lprintf(T_ADDR, "%s\n", dump_x25_addr(data + 0));
		data   += ROSE_ADDR_LEN + ROSE_ADDR_LEN;
		length -= ROSE_ADDR_LEN + ROSE_ADDR_LEN;
		data_dump(data, length, 1);
		return;

	case CALL_ACCEPTED:
		lprintf(T_ROSEHDR, "CALL ACCEPTED\n");
		return;

	case CLEAR_REQUEST:
		lprintf(T_ROSEHDR, "CLEAR REQUEST - Cause %s - Diag %d\n",
			clear_code(data[3]), data[4]);
		return;

	case CLEAR_CONFIRMATION:
		lprintf(T_ROSEHDR, "CLEAR CONFIRMATION\n");
		return;

	case DIAGNOSTIC:
		lprintf(T_ROSEHDR, "DIAGNOSTIC - Diag %d\n", data[3]);
		return;

	case INTERRUPT:
		lprintf(T_ROSEHDR, "INTERRUPT\n");
		data_dump(data + 3, length - 3, hexdump);
		return;

	case INTERRUPT_CONFIRMATION:
		lprintf(T_ROSEHDR, "INTERRUPT CONFIRMATION\n");
		return;

	case RESET_REQUEST:
		lprintf(T_ROSEHDR, "RESET REQUEST - Cause %s - Diag %d\n",
			reset_code(data[3]), data[4]);
		return;

	case RESET_CONFIRMATION:
		lprintf(T_ROSEHDR, "RESET CONFIRMATION\n");
		return;
		
	case RESTART_REQUEST:
		lprintf(T_ROSEHDR, "RESTART REQUEST - Cause %s - Diag %d\n",
			restart_code(data[3]), data[4]);
		return;
		
	case RESTART_CONFIRMATION:
		lprintf(T_ROSEHDR, "RESTART CONFIRMATION\n");
		return;

	case REGISTRATION_REQUEST:
		lprintf(T_ROSEHDR, "REGISTRATION REQUEST\n");
		return;
		
	case REGISTRATION_CONFIRMATION:
		lprintf(T_ROSEHDR, "REGISTRATION CONFIRMATION\n");
		return;
	}

	if ((data[2] & 0x01) == DATA) {
		lprintf(T_ROSEHDR, "DATA R%d S%d %s%s%s\n",
			(data[2] >> 5) & 0x07, (data[2] >> 1) & 0x07,
			(data[0] & QBIT) ? "Q" : "",
			(data[0] & DBIT) ? "D" : "",
			(data[2] & MBIT) ? "M" : "");
		data_dump(data + 3, length - 3, hexdump);
		return;
	}

	switch (data[2] & 0x1F) {
		case RR:
			lprintf(T_ROSEHDR, "RR R%d\n", (data[2] >> 5) & 0x07);
			return;
		case RNR:
			lprintf(T_ROSEHDR, "RNR R%d\n", (data[2] >> 5) & 0x07);
			return;
		case REJ:
			lprintf(T_ROSEHDR, "REJ R%d\n", (data[2] >> 5) & 0x07);
			return;
	}

	lprintf(T_ROSEHDR, "UNKNOWN\n");
	data_dump(data, length, 1);
}

static char *clear_code(unsigned char code)
{
	static char buffer[25];

	if (code == 0x00 || (code & 0x80) == 0x80)
		return "DTE Originated";
	if (code == 0x01)
		return "Number Busy";
	if (code == 0x09)
		return "Out Of Order";
	if (code == 0x11)
		return "Remote Procedure Error";
	if (code == 0x19)
		return "Reverse Charging Acceptance Not Subscribed";
	if (code == 0x21)	
		return "Incompatible Destination";
	if (code == 0x29)
		return "Fast Select Acceptance Not Subscribed";
	if (code == 0x39)
		return "Destination Absent";
	if (code == 0x03)
		return "Invalid Facility Requested";
	if (code == 0x0B)
		return "Access Barred";
	if (code == 0x13)
		return "Local Procedure Error";
	if (code == 0x05)
		return "Network Congestion";
	if (code == 0x0D)
		return "Not Obtainable";
	if (code == 0x15)
		return "RPOA Out Of Order";
	
	sprintf(buffer, "Unknown %02X", code);
	
	return buffer;
}

static char *reset_code(unsigned char code)
{
	static char buffer[25];

	if (code == 0x00 || (code & 0x80) == 0x80)
		return "DTE Originated";
	if (code == 0x03)
		return "Remote Procedure Error";
	if (code == 0x11)	
		return "Incompatible Destination";
	if (code == 0x05)
		return "Local Procedure Error";
	if (code == 0x07)
		return "Network Congestion";
	
	sprintf(buffer, "Unknown %02X", code);
	
	return buffer;
}

static char *restart_code(unsigned char code)
{
	static char buffer[25];

	if (code == 0x00 || (code & 0x80) == 0x80)
		return "DTE Originated";
	if (code == 0x01)
		return "Local Procedure Error";
	if (code == 0x03)
		return "Network Congestion";
	if (code == 0x07)
		return "Network Operational";
	
	sprintf(buffer, "Unknown %02X", code);
	
	return buffer;
}

static char *dump_x25_addr(unsigned char *data)
{
	static char buffer[25];

	sprintf(buffer, "%02X%02X%02X%02X%02X", data[0], data[1], data[2], data[3], data[4]);

	return buffer;
}

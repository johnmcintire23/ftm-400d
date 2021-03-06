// Copyright 2014-2016, Kurt Zeilenga. All rights reserved.
//
// Permission to use, copy, modify, and/or distribute this software
// for any purpose with or without fee is hereby granted, provided
// that the above copyright notice and this permission notice appear
// in all copies.
//
// Provided "as is" without warranty of any kind.

//
// Yaesu FTM 400dr memory dump to XML program
//

#include <memory>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cstdlib>
#include <cassert>
#include <cstring>

#include "ftm.h"

using namespace std;

static void encodeChannel(
	const Channel * c,
	unsigned char * dbuf,
	unsigned char * sbuf)
{
	memset(sbuf, Channel::TAG_FILL, Channel::TAG_SIZE);
	memset(dbuf, 0, Channel::CHANNEL_SIZE);

	if (!c) return;

#ifdef CHARSET_EXPERIMENT
	if (c->bank == 2 && c->slot>100) {
		memcpy(sbuf, c->tag.c_str(), Channel::TAG_SIZE);

	} else
#endif
	{
		str2data(c->tag, sbuf);
	}

	dbuf[0] |= 0x60U & (c->scan << 5);

	if (c->band) {
		dbuf[0] |= 0x07U & (c->band-1);

	} else if (c->rx > 300*1000) {
		dbuf[0] |= 0x03U; /* UHF */

	} else {
		dbuf[0] |= 0x01U; /* VHF */
	}

	{
		unsigned x = c->rx;
		unsigned rem = x % 10;
		if (rem >=  5) {
			dbuf[2] |= 0x80U;		// 5Hz
		}
		x /= 10;
		rem = x % 10;
		dbuf[4] |= 0x0fU & rem;		// 10Hz
		x /= 10;
		rem = x % 10;
		dbuf[4] |= 0xf0U & (rem<<4);// 100Hz
		x /= 10;
		rem = x % 10;
		dbuf[3] |= 0x0fU & rem;		// Mhz
		x /= 10;
		rem = x % 10;
		dbuf[3] |= 0xf0U & (rem<<4);// 10Mhz
		x /= 10;
		rem = x % 10;
		dbuf[2] |= 0x0fU & rem;		// 100Mhz
	}

	if (c->tx) {
		unsigned x = c->tx;
		unsigned rem = x % 10;
		if (rem >=  5) {
			dbuf[6] |= 0x80U;		// 5Hz
		}
		x /= 10;
		rem = x % 10;
		dbuf[8] |= 0x0fU & rem;		// 10Hz
		x /= 10;
		rem = x % 10;
		dbuf[8] |= 0xf0U & (rem<<4);// 100Hz
		x /= 10;
		rem = x % 10;
		dbuf[7] |= 0x0fU & rem;		// Mhz
		x /= 10;
		rem = x % 10;
		dbuf[7] |= 0xf0U & (rem<<4);// 10Mhz
		x /= 10;
		rem = x % 10;
		dbuf[6] |= 0x0fU & rem;		// 100Mhz

		dbuf[1] |= 0x04U;

	} else if (c->duplex > 0) {
		dbuf[1] |= 0x03U; // +

	} else if (c->duplex < 0) {
		dbuf[1] |= 0x02U; // -
	}

	dbuf[1] |= 0x70U & (c->mode << 4);
	dbuf[5] |= 0xf0U & (c->sql << 4);
	dbuf[9] |= 0xc0U & (c->power << 6);
	dbuf[9] |= 0x1fU & c->tone;
	dbuf[10] |= 0x1fU & c->dcs;

	dbuf[11] |= 0x0fU;
	if (c->bank < 2) dbuf[11] |= 0x80U;

	// offset size in 50khz steps, hence
	//		0x64 * 50khz = 5000mhz
	// 		0x0c * 50khz = 600mhz
	// may bleed into c[12] as manual says range is 0 - 99.95mhz,
	// that's 0-2000 (0x7CF) steps
	//
	// values here are good for the US, YMMV.
	if (c->duplex && !c->tx) {
		unsigned offset = c->offset/50;
		dbuf[13] |= 0xFF & offset;
		// range is 0 - 0x7EFU
		// need to confirm where top bits go
		// dbuf[14] |= 0x07 & (offset >> 8);
	}

	dbuf[0] |= 0x80U; // programmed 
}
 
static Channel * parseChannel(
	xmlDoc * doc,
	xmlNs * ns,
	xmlNode * node)
{
	auto_ptr<Channel> c(new Channel);
	const char * str;

	str = (const char *) xmlGetProp(node, (const xmlChar *)"name");
	if (str) {
		c->memname = str;
		cout << "Channel: " << c->memname << endl;

	} else {
		str = (const char *) xmlGetProp(node, (const xmlChar *)"bank");
		if (str) {
			c->bank = strtol(str, NULL, 10);
		}

		str = (const char *) xmlGetProp(node, (const xmlChar *)"slot");
		if (str) {
			c->slot = strtol(str, NULL, 10);
		}

		cout << "Channel: " << c->bank << "/" << c->slot << endl;
	}

	for (xmlNode * cur = node->xmlChildrenNode; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE) {
			continue;
		}
		if (cur->ns != ns) {
			cerr << TAB "Skipping " << cur->name
				<< " ns=" << (cur->ns ? (const char *)cur->ns->href : "<empty>") << endl;
			continue;
		}

		const char * str = (const char *) xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);

		cout << TAB << cur->name << TAB << (str ? (const char *)str : "nil") << endl;

		if (!strcmp((const char *)cur->name, "band")) {
			for (int i=1; bands[i]; i++) {
				if (!strcasecmp(str, bands[i])) {
					c->band = i;
					break;	
				}
			}
			cout << TAB "band=" << bands[c->band] << " " << c->band << endl;

		} else if (!strcmp((const char *)cur->name, "frequency")) {
			char *p = NULL;
			long l = strtol(str, &p, 10);
			c->rx = l*1000;

			if (p && *p == '.') {
				const static int m[3] = {100, 10, 1};
				p++;
				for (int i=0; i<3; ++i) {
					if (!p[i]) break;
					c->rx += m[i] * (p[i] - '0');
				}
			}

			cout << TAB "rx=" << c->rx << endl;

		} else if (!strcmp((const char *)cur->name, "txFrequency")) {
			char *p = NULL;
			long l = strtol(str, &p, 10);
			c->tx = l*1000;
			if (p && *p == '.') {
				const static int m[3] = {100, 10, 1};
				p++;
				for (int i=0; i<3; ++i) {
					if (!p[i]) break;
					c->tx += m[i] * (p[i] - '0');
				}
			}

			cout << TAB "tx=" << c->tx << endl;

		} else if (!strcmp((const char *)cur->name, "offset")) {
			if (!strcmp(str, "+")) {
				c->duplex = +1;
				cout << TAB "offset=" << "+" << endl;

			} else if (!strcmp(str, "-")) {
				c->duplex = -1;
				cout << TAB "offset=" << "-" << endl;

			} else {
				c->duplex = +1;
				if (*str == '-') {
					c->duplex=-1;
					++str;

				} else if (*str == '+') {
					++str;
				}
				char *p = NULL;
				long l = strtol(str, &p, 10);
				c->offset = l*1000;
				if (p && *p == '.') {
					const static int m[3] = {100, 10, 1};
					p++;
					for (int i=0; i<3; ++i) {
						if (!p[i]) break;
						c->offset += m[i] * (p[i] - '0');
					}
				}

				cout << TAB "offset=" << (c->offset > 0 ? "+" : "-") << c->offset << endl;
			}


		} else if (!strcmp((const char *)cur->name, "sql")) {
			for (int i=1; sqls[i]; i++) {
				if (!strcasecmp((const char *)str, sqls[i])) {
					c->sql = i;
					break;
				}
			}

			if (!c->sql) {
				cerr << TAB "Bad SQL: " << str << endl;
			}
			cout << TAB "sql=" << c->sql << endl;

		} else if (!strcmp((const char *)cur->name, "tone")) {
			long l = strtol(str, NULL, 10);
			c->tone=-1;

			switch (l) {
			case 67: c->tone = 0; break;
			case 69: c->tone = 1; break;
			case 71: c->tone = 2; break;
			case 74: c->tone = 3; break;
			case 77: c->tone = 4; break;
			case 79: c->tone = 5; break;
			case 82: c->tone = 6; break;
			case 85: c->tone = 7; break;
			case 88: c->tone = 8; break;
			case 91: c->tone = 9; break;
			case 94: c->tone = 10; break;
			case 97: c->tone = 11; break;
			case 100: c->tone = 12; break;
			case 103: c->tone = 13; break;
			case 107: c->tone = 14; break;
			case 110: c->tone = 15; break;
			case 114: c->tone = 16; break;
			case 118: c->tone = 17; break;
			case 123: c->tone = 18; break;
			case 127: c->tone = 19; break;
			case 131: c->tone = 20; break;
			case 136: c->tone = 21; break;
			case 141: c->tone = 22; break;
			case 146: c->tone = 23; break;
			case 151: c->tone = 24; break;
			case 156: c->tone = 25; break;
			case 162: c->tone = 26; break;
			case 167: c->tone = 27; break;
			case 173: c->tone = 28; break;
			case 179: c->tone = 29; break;
			case 186: c->tone = 30; break;
			case 192: c->tone = 31; break;
			case 203: c->tone = 32; break;
			case 206: c->tone = 33; break;
			case 210: c->tone = 34; break;
			case 218: c->tone = 35; break;
			case 225: c->tone = 36; break;
			case 229: c->tone = 37; break;
			case 233: c->tone = 38; break;
			case 241: c->tone = 39; break;
			case 250: c->tone = 40; break;
			case 254: c->tone = 41; break;
			default:
				cerr << TAB "Bad tone: " << str << endl;
			}

			if (c->tone < 0) {
				c->tone = 12;
			}
			cout << TAB "tone=" << tones[c->tone] << " (" << c->tone << ")" << endl;

		} else if (!strcmp((const char *)cur->name, "dcs")) {
			for (int i=1; dcsCodes[i]; i++) {
				if (!strcasecmp((const char *)str, dcsCodes[i])) {
					c->dcs = i;
					break;
				}
			}

			if (!c->dcs) {
				cerr << TAB "Bad DCS: " << str << endl;
			}
			cout << TAB "dcs=" << c->dcs << endl;

		} else if (!strcmp((const char *)cur->name, "mode")) {
			for (int i=0; modes[i]; i++) {
				if (!strcasecmp((const char *)str, modes[i])) {
					c->mode = i;
					break;
				}
			}
			cout << TAB "mode=" << c->mode << endl;

		} else if (!strcmp((const char *)cur->name, "power")) {
			if (!strcasecmp(str, "high")) {
				c->power = 0;
			} else if (!strcasecmp(str, "medium")) {
				c->power = 1;
			} else if (!strcasecmp(str, "low")) {
				c->power = 2;
			}
			cout << TAB "power=" << c->power << endl;

		} else if (!strcmp((const char *)cur->name, "name") && str) {
			if (c->tag.empty()) {
				c->tag = str;
				if (c->tag.length() > Channel::TAG_SIZE) c->tag.resize(Channel::TAG_SIZE);
				cout << TAB "name=" << c->tag << endl;

			} else {
				cout << TAB "name ignored (have tag)" << endl;
			}

		} else if (!strcmp((const char *)cur->name, "tag") && str) {
			c->tag = str;
			if (c->tag.length() > Channel::TAG_SIZE) c->tag.resize(Channel::TAG_SIZE);
			cout << TAB "tag=" << c->tag << endl;

		} else if (!strcmp((const char *)cur->name, "scan")) {
			for (int i=1; scans[i]; i++) {
				if (!strcasecmp((const char *)str, scans[i])) {
					c->scan = i;
					break;
				}
			}

			if (!c->scan) {
				cerr << TAB "Bad scan: " << str << endl;
			}
			cout << TAB "scan=" << c->scan << endl;
		}
	}

	if (c->duplex && !c->offset) {
		// use US defaults
		if (c->rx > 300) {
			c->offset = 5000;
		} else {
			c->offset = 600;
		}
	}

	return c.release();
}

static void processDoc(
	xmlDoc * doc,
	unsigned char * data)
{
	xmlNode * root = xmlDocGetRootElement(doc);
	xmlNs * ns = NULL;

	if (root->ns) {
		if (strcmp((const char*)root->ns->href, SCHEMA_NS_URI)) {
			cerr << "Bad NS URI: " << root->ns->href << endl;
			return;
		}
		ns = root->ns;
	}

	if (strcmp((const char *)root->name, "channels")) {
		cerr << "Bad root element: " << root->ns->href << endl;
		return;
	}

	int n=101;
	for (xmlNode * node = root->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE) {
			continue;
		}
		if (node->ns != ns) {
			cerr << "Skipping " << node->name
				<< " ns=" << (node->ns ? (const char *)node->ns->href : "<empty>") << endl;
			continue;
		}
		if (strcmp((const char *)node->name, "channel")) {
			cerr << "Skipping " << node->name
				<< " ns=" << (node->ns ? (const char *)node->ns->href : "<empty>") << endl;
			continue;
		}

		auto_ptr<Channel> chn(parseChannel(doc, ns, node));
		if (!chn.get()) {
			cerr << "Could not parse channel" << endl;
		}

		vector<unsigned char> cdata(Channel::CHANNEL_SIZE);
		vector<unsigned char> sdata(Channel::TAG_SIZE);

		int slot = chn->slot ? chn->slot : n++;
		slot--;

		unsigned char * d;
		unsigned char * s;

		if (chn->memname.length()) {
			int i;

			for(i=0; i<Channel::NPCHANNELS; i++) {
				if (!strcmp(pchannels[i], chn->memname.c_str())) break;
			}

			if (i < Channel::NPCHANNELS) {
				d = &data[Channel::PCHANNEL_OFFSET + (i * Channel::CHANNEL_SIZE)];
				s = &data[Channel::PCHANNEL_TAG_OFFSET + (i * Channel::TAG_SIZE)];

			} else {
				// assume its "Home"
				d = &data[Channel::HOME_OFFSET];
				s = &data[Channel::HOME_TAG_OFFSET];
			}

		} else if (chn->bank < 2) {
			d = &data[Channel::CHANNEL_TOP_OFFSET + (slot * Channel::CHANNEL_SIZE)];
			s = &data[Channel::CHANNEL_TOP_TAG_OFFSET + (slot * Channel::TAG_SIZE)];

			if (slot>=Channel::NCHANNELS) {
				cerr << "too many bank 1 channels, skipping";
				continue;
			}

		} else {
			d = &data[Channel::CHANNEL_BOT_OFFSET + (slot * Channel::CHANNEL_SIZE)];
			s = &data[Channel::CHANNEL_BOT_TAG_OFFSET + (slot * Channel::TAG_SIZE)];

			if (slot>=Channel::NCHANNELS) {
				cerr << "too many bank 1 channels, skipping";
				continue;
			}
		}

		encodeChannel(chn.get(), d, s);

#ifdef CHARSET_EXPERIMENT
		if (chn->bank==2 && chn->slot==100) {
			for(unsigned i=1; i<256; i++) {
				slot = chn->slot=100+i;
				slot--;
				unsigned char c=i;
				chn->tag.assign(8, * reinterpret_cast<char *>(&c));

				d = &data[Channel::CHANNEL_BOT_OFFSET + (slot * Channel::CHANNEL_SIZE)];
				s = &data[Channel::CHANNEL_BOT_TAG_OFFSET + (slot * Channel::TAG_SIZE)];

				encodeChannel(chn.get(), d, s);
			}
		}
#endif
	}
}

int main(int argc, char *argv[])
{
	const char * xmlfile = argv[1];
	const char * infile = argv[2];
	const char * outfile = argv[3];

	if (argc != 4) {
		cerr << "expects three arguments: xml indata outdata" << endl;
		return EXIT_FAILURE;
	}

	ifstream is;

	is.open(xmlfile, ios::binary);
	if (!is.is_open()) {
		cerr << "Failed to open " << xmlfile << " for input." << endl;
		return EXIT_FAILURE;
	}

	is.seekg(0, ios::end);
	off_t xmllen = is.tellg();
	vector<char> xml(xmllen);

	is.seekg(0, ios::beg);
	is.read((&xml[0]), xmllen);
	is.close();

	is.open(infile, ios::binary);
	if (!is.is_open()) {
		cerr << "Failed to open " << infile << " for input." << endl;
		return EXIT_FAILURE;
	}

	is.seekg(0, ios::end);
	off_t datalen = is.tellg();
	vector<unsigned char> input(datalen);

	is.seekg(0, ios::beg);
	is.read(reinterpret_cast<char *>(&input[0]), datalen);
	is.close();

	if (datalen != 25600) { 
		cerr << "bad input data length: " << datalen << endl;
		return EXIT_FAILURE;
	}

	vector<unsigned char> output(input);

	xmlDoc * doc = xmlReadMemory(&xml[0], xmllen, xmlfile, NULL, 0);
	if (!doc) {
		cerr << "Failed to parse " << xmlfile << endl;
		return EXIT_FAILURE;
	}

	processDoc(doc, &output[0]);
	xmlFreeDoc(doc);

	ofstream os;
	os.open(outfile, ios::binary);

	if (!os.is_open()) {
		cerr << "Failed to open " << outfile << endl;
		return EXIT_FAILURE;
	}

	os.write(reinterpret_cast<char *>(&output[0]), output.size());
	os.close();

	xmlCleanupParser();
	return EXIT_SUCCESS;
}

#include "DMGDisk.h"
#include <stdexcept>
#include "be.h"
#include <iostream>
#include <cstring>
#include <ctype.h>
#include <memory>
#include <sstream>
#include "DMGPartition.h"
#include "AppleDisk.h"
#include "GPTDisk.h"
#include "CachedReader.h"
#include "exceptions.h"

DMGDisk::DMGDisk(std::shared_ptr<Reader> reader)
	: m_reader(reader), m_zone(40000)
{
	uint64_t offset = m_reader->length();
	UDIFResourceFile udif;

	if (offset < 512)
		throw io_error("File to small to be a DMG");

	offset -= 512;

	if (m_reader->read(&udif, sizeof(udif), offset) != sizeof(udif))
		throw io_error("Cannot read the KOLY block");

	if (be(udif.fUDIFSignature) != UDIF_SIGNATURE)
		throw io_error("Invalid KOLY block signature");
	
	loadKoly(udif);
}

DMGDisk::~DMGDisk()
{
	xmlFreeDoc(m_kolyXML);
}

bool DMGDisk::isDMG(std::shared_ptr<Reader> reader)
{
	uint64_t offset = reader->length() - 512;
	decltype(UDIFResourceFile::fUDIFSignature) sig = 0;

	reader->read(&sig, sizeof(sig), offset);
	return be(sig) == UDIF_SIGNATURE;
}

void DMGDisk::loadKoly(const UDIFResourceFile& koly)
{
	std::unique_ptr<char[]> xmlData;
	xmlXPathContextPtr xpathContext;
	xmlXPathObjectPtr xpathObj;
	uint64_t offset, length;
	bool simpleWayOK = false;

	offset = be(koly.fUDIFXMLOffset);
	length = be(koly.fUDIFXMLLength);

	xmlData.reset(new char[length]);
	m_reader->read(xmlData.get(), length, offset);

	m_kolyXML = xmlParseMemory(xmlData.get(), length);

//#if 0 // Asian copies of OS X put crap UTF characters into XML data making type/name parsing unreliable
	xpathContext = xmlXPathNewContext(m_kolyXML);

	// select all partition dictionaries with partition ID >= 0
	xpathObj = xmlXPathEvalExpression((const xmlChar*) "/plist/dict/key[text()='resource-fork']/following-sibling::dict[1]/key[text()='blkx']"
			"/following-sibling::array[1]/dict[key[text()='ID']/following-sibling::string[text() >= 0]]", xpathContext);

	if (xpathObj && xpathObj->nodesetval)
		simpleWayOK = loadPartitionElements(xpathContext, xpathObj->nodesetval);
	
	xmlXPathFreeObject(xpathObj);
	xmlXPathFreeContext(xpathContext);
//#else
	
	if (!simpleWayOK)
	{
		std::shared_ptr<Reader> rm1, r1;
		PartitionedDisk* pdisk;

		rm1 = readerForKolyBlock(-1);

		if (AppleDisk::isAppleDisk(rm1))
		{
			r1 = readerForKolyBlock(0); // TODO: this is not always partition 0
			pdisk = new AppleDisk(rm1, r1);
		}
		else if (GPTDisk::isGPTDisk(rm1))
		{
			r1 = readerForKolyBlock(1);
			pdisk = new GPTDisk(rm1, r1);
		}
		else
			throw function_not_implemented_error("Unknown partition table type");

		m_partitions = pdisk->partitions();

		delete pdisk;
	}
//#endif
}

bool DMGDisk::loadPartitionElements(xmlXPathContextPtr xpathContext, xmlNodeSetPtr nodes)
{
	for (int i = 0; i < nodes->nodeNr; i++)
	{
		xmlXPathObjectPtr xpathObj;
		Partition part;
		BLKXTable* table;

		if (nodes->nodeTab[i]->type != XML_ELEMENT_NODE)
			continue;

		xpathContext->node = nodes->nodeTab[i];

		xpathObj = xmlXPathEvalExpression((const xmlChar*) "string(key[text()='CFName']/following-sibling::string)", xpathContext);
		
		if (!xpathObj || !xpathObj->stringval)
			xpathObj = xmlXPathEvalExpression((const xmlChar*) "string(key[text()='Name']/following-sibling::string)", xpathContext);

		if (!xpathObj || !xpathObj->stringval)
			throw io_error("Invalid XML data, partition Name key not found");
		
		table = loadBLKXTableForPartition(i);
		
		if (table)
		{
			part.offset = be(table->firstSectorNumber) * 512;
			part.size = be(table->sectorCount) * 512;
		}

		if (!parseNameAndType((const char*) xpathObj->stringval, part.name, part.type) && m_partitions.empty())
			return false;
		m_partitions.push_back(part);

		xmlXPathFreeObject(xpathObj);
		//delete table;
	}
	
	return true;
}

bool DMGDisk::parseNameAndType(const std::string& nameAndType, std::string& name, std::string& type)
{
	// Format: "Apple (Apple_partition_map : 1)"
	size_t paren = nameAndType.find('(');
	size_t colon, space;

	if (paren == std::string::npos)
		return false;

	name = nameAndType.substr(0, paren-1);
	colon = nameAndType.find(':', paren);

	if (colon == std::string::npos)
		return false;

	type = nameAndType.substr(paren+1, (colon - paren) - 1);
	space = type.rfind(' ');
	
	if (space != std::string::npos && space == type.length()-1)
		type.resize(type.length() - 1); // remove space at the end
	
	return true;
}

BLKXTable* DMGDisk::loadBLKXTableForPartition(int index)
{
	xmlXPathContextPtr xpathContext;
	xmlXPathObjectPtr xpathObj;
	char expr[300];
	BLKXTable* rv = nullptr;

	sprintf(expr, "string(/plist/dict/key[text()='resource-fork']/following-sibling::dict[1]/key[text()='blkx']"
		"/following-sibling::array[1]/dict[key[text()='ID']/following-sibling::string[text() = %d]]/key[text()='Data']/following-sibling::data)", index);

	xpathContext = xmlXPathNewContext(m_kolyXML);
	xpathObj = xmlXPathEvalExpression((const xmlChar*) expr, xpathContext);

	if (xpathObj && xpathObj->stringval && *xpathObj->stringval)
	{
		// load data from base64
		std::vector<uint8_t> data;
		
		base64Decode((char*)xpathObj->stringval, data);
		rv = static_cast<BLKXTable*>(operator new(data.size()));
		
		memcpy(rv, &data[0], data.size());
	}

	xmlXPathFreeObject(xpathObj);
	xmlXPathFreeContext(xpathContext);
	
	return rv;
}

/*
 * The following base64 code is based on code posted by LihO at
 * http://stackoverflow.com/questions/180947/base64-decode-snippet-in-c,
 * which in turn was based on code written by René Nyffenegger at
 * https://github.com/ReneNyffenegger/cpp-base64.
 *
 * His original licensing terms are included here:
 *
 * Copyright (C) 2004-2017 René Nyffenegger
 *
 * This source code is provided 'as-is', without any express or implied
 * warranty. In no event will the author be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this source code must not be misrepresented; you must not
 *    claim that you wrote the original source code. If you use this source code
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original source code.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * René Nyffenegger rene.nyffenegger@adp-gmbh.ch
 */
static inline bool is_base64(uint8_t c) {
	return (isalnum(c) || (c == '+') || (c == '/'));
}

bool DMGDisk::base64Decode(const std::string& input, std::vector<uint8_t>& output)
{
	static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	
	const int in_len_const = input.size();
	int in_len = input.size();
	int i = 0;
	int j = 0;
	int in_ = 0;
	uint8_t char_array_4[4], char_array_3[3];
	
	while (in_len-- && (input[in_] != '='))
	{
		while (!is_base64(input[in_])) // this loop skips non base64 characters
		{
			if (in_ < in_len_const - 1)
			{
				in_++;
				continue;
			}
			else 
				break;
		}

		if (in_ == in_len_const - 1)
			break;

		char_array_4[i++] = input[in_]; in_++;
		if (i == 4) 
		{
			for (i = 0; i < 4; i++)
				char_array_4[i] = base64_chars.find(char_array_4[i]);

			char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
			char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
			char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

			for (i = 0; (i < 3); i++)
				output.push_back(char_array_3[i]);
			i = 0;
		}
	}

	if (i) 
	{
		for (j = i; j < 4; j++)
			char_array_4[j] = 0;

		for (j = 0; j < 4; j++)
			char_array_4[j] = base64_chars.find(char_array_4[j]);

		char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
		char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
		char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

		for (j = 0; (j < i - 1); j++) 
			output.push_back(char_array_3[j]);
	}

	return (output.size() > 0);
}

std::shared_ptr<Reader> DMGDisk::readerForPartition(int index)
{
	for (int i = -1;; i++)
	{
		BLKXTable* table = loadBLKXTableForPartition(i);
		
		if (!table)
			continue;
		
		if (be(table->firstSectorNumber)*512 == m_partitions[index].offset)
		{
			std::stringstream partName;
			partName << "part-" << index;

			return std::shared_ptr<Reader>(
						new CachedReader(std::shared_ptr<Reader>(new DMGPartition(m_reader, table)), &m_zone, partName.str())
			);
		}
		
		delete table;
	}
	
	return nullptr;
}

std::shared_ptr<Reader> DMGDisk::readerForKolyBlock(int index)
{
	BLKXTable* table = loadBLKXTableForPartition(index);
	if (!table)
		return nullptr;
	return std::shared_ptr<Reader>(new DMGPartition(m_reader, table));
}


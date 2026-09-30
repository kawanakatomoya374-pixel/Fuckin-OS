/*
 * This file is part of LibParserUtils.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2007 John-Mark Bell <jmb@netsurf-browser.org>
 */

#ifndef parserutils_charset_mibenum_h_
#define parserutils_charset_mibenum_h_

#ifdef __cplusplus
extern "C"
{
#endif

#include <inttypes.h>
#include <stdbool.h>

#include <parserutils/errors.h>
#include <parserutils/functypes.h>

/* Convert an encoding alias to a MIB enum value */
uint16_t parserutils_charset_mibenum_from_name(const char *alias, size_t len);
/* Convert a MIB enum value into an encoding alias */
const char *parserutils_charset_mibenum_to_name(uint16_t mibenum);
/* Determine if a MIB enum value represents a Unicode variant */
bool parserutils_charset_mibenum_is_unicode(uint16_t mibenum);

/* MIBENUM_IS_UNICODE was referenced by src/charset/aliases.c but never
 * actually defined anywhere in this library (upstream gap, not
 * something introduced while porting this to C-OS - grep the
 * original tree and it's genuinely missing). IANA's MIB enum registry
 * reserves 1000-1020 for the Unicode-family charsets (UCS-2/4, UTF-7,
 * UTF-16 and its BE/LE forms, CESU-8, UTF-32 and its BE/LE forms,
 * SCSU, BOCU-1); UTF-8 (106) and the older UNICODE-1-1-UTF-7 (103)
 * registration predate that block and are called out explicitly. */
#define MIBENUM_IS_UNICODE(mibenum) \
	(((mibenum) >= 1000 && (mibenum) <= 1020) || (mibenum) == 103 || (mibenum) == 106)

#ifdef __cplusplus
}
#endif

#endif

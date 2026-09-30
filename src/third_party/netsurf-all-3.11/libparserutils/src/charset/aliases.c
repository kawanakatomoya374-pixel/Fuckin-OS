/*
 * This file is part of LibParserUtils.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2007 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include "charset/aliases.h"
#include "utils/utils.h"

/* Bring in the aliases tables */
#include "aliases.inc"

typedef struct {
        size_t slen;
        const char *s;
} lengthed_string;


#define IS_PUNCT_OR_SPACE(x)                    \
        (!(((x) >= 'A' && (x) <= 'Z') ||        \
           ((x) >= 'a' && (x) <= 'z') ||        \
           ((x) >= '0' && (x) <= '9')))


static int parserutils_charset_alias_match(const void *a, const void *b)
{
        lengthed_string *s = (lengthed_string *)a;
        parserutils_charset_aliases_alias *alias = (parserutils_charset_aliases_alias*)b;
        size_t key_left = s->slen;
        size_t alias_left = alias->name_len;
        const char *s_alias = alias->name;
        const char *s_key = s->s;
        int cmpret;
        
        while ((key_left > 0) && (alias_left > 0)) {
                while ((key_left > 0) && IS_PUNCT_OR_SPACE(*s_key)) {
                        key_left--; s_key++;
                }
                while ((alias_left > 0) && IS_PUNCT_OR_SPACE(*s_alias)) {
                        alias_left--; s_alias++;
                }

                if (key_left == 0 || alias_left == 0)
                        break;
                
                /* Fold both operands: alias table entries retain canonical\n                 * case (for example, \"UTF-8\"), while lookup is defined\n                 * to be case-insensitive. */
                cmpret = tolower((unsigned char)*s_key) -
                         tolower((unsigned char)*s_alias);
                
                if (cmpret != 0) {
                        return cmpret;
                }
                
                key_left--;
                s_key++;
                alias_left--;
                s_alias++;
        }
        
        while ((key_left > 0) && IS_PUNCT_OR_SPACE(*s_key)) {
          key_left--; s_key++;
        }
        while ((alias_left > 0) && IS_PUNCT_OR_SPACE(*s_alias)) {
          alias_left--; s_alias++;
        }

        return key_left - alias_left;
}

/**
 * Retrieve the canonical form of an alias name
 *
 * \param alias  The alias name
 * \param len    The length of the alias name
 * \return Pointer to canonical form or NULL if not found
 */
parserutils_charset_aliases_canon *parserutils__charset_alias_canonicalise(
		const char *alias, size_t len)
{
        parserutils_charset_aliases_alias *c;
        lengthed_string s;
        
        s.slen = len;
        s.s = alias;

        /* The generated alias list contains canonical mixed-case spellings,
         * while lookup is deliberately case- and punctuation-insensitive.
         * Its source order is therefore not a strict ordering for that
         * comparator, so bsearch can miss valid aliases such as UTF-16.
         * Charset setup is infrequent; a bounded exact linear comparison of
         * this small static table is deterministic and correct. */
        for (size_t i = 0; i < charset_aliases_count; ++i) {
                if (parserutils_charset_alias_match(&s, &charset_aliases[i]) == 0) {
                        c = &charset_aliases[i];
                        return c->canon;
                }
        }

        return NULL;
}

/**
 * Retrieve the MIB enum value assigned to an encoding name
 *
 * \param alias  The alias to lookup
 * \param len    The length of the alias string
 * \return The MIB enum value, or 0 if not found
 */
uint16_t parserutils_charset_mibenum_from_name(const char *alias, size_t len)
{
	parserutils_charset_aliases_canon *c;

	if (alias == NULL)
		return 0;

	c = parserutils__charset_alias_canonicalise(alias, len);
	if (c == NULL)
		return 0;

	return c->mib_enum;
}

/**
 * Retrieve the canonical name of an encoding from the MIB enum
 *
 * \param mibenum The MIB enum value
 * \return Pointer to canonical name, or NULL if not found
 */
const char *parserutils_charset_mibenum_to_name(uint16_t mibenum)
{
	int i;
	parserutils_charset_aliases_canon *c;
        
        for (i = 0; i < charset_aliases_canon_count; ++i) {
                c = &canonical_charset_names[i];
                if (c->mib_enum == mibenum)
                        return c->name;
        }
        
        return NULL;
}

/**
 * Detect if a parserutils_charset is Unicode
 *
 * \param mibenum  The MIB enum to consider
 * \return true if a Unicode variant, false otherwise
 */
bool parserutils_charset_mibenum_is_unicode(uint16_t mibenum)
{
        return MIBENUM_IS_UNICODE(mibenum);
}

// Module:  Log4CPLUS
// File:    property.cxx
// Created: 2/2002
// Author:  Tad E. Smith
//
//
// Copyright 2002-2017 Tad E. Smith
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <log4cplus/config.hxx>

#include <cstring>
#if defined (UNICODE)
#  include <cwctype>
#else
#  include <cctype>
#endif
#if defined (LOG4CPLUS_HAVE_CODECVT_UTF8_FACET) \
    || defined (LOG4CPLUS_HAVE_CODECVT_UTF16_FACET) \
    || defined (LOG4CPLUS_HAVE_CODECVT_UTF32_FACET)
#  include <codecvt>
#endif
#include <locale>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <log4cplus/streams.h>
#include <log4cplus/fstreams.h>
#include <log4cplus/helpers/stringhelper.h>
#include <log4cplus/helpers/property.h>
#include <log4cplus/internal/internal.h>
#include <log4cplus/internal/env.h>
#include <log4cplus/helpers/loglog.h>
#include <log4cplus/exception.h>
#include <log4cplus/configurator.h>

#if defined (LOG4CPLUS_WITH_UNIT_TESTS)
#include <catch_amalgamated.hpp>
#endif


namespace log4cplus::helpers {


const tchar Properties::PROPERTIES_COMMENT_CHAR = LOG4CPLUS_TEXT('#');


namespace
{


static
int
is_space (tchar ch)
{
#if defined (UNICODE)
    return std::iswspace (ch);
#else
    return std::isspace (static_cast<unsigned char>(ch));
#endif
}


static
void
trim_leading_ws (tstring & str)
{
    tstring::iterator it = str.begin ();
    for (; it != str.end (); ++it)
    {
        if (! is_space (*it))
            break;
    }
    str.erase (str.begin (), it);
}


static
void
trim_trailing_ws (tstring & str)
{
    tstring::reverse_iterator rit = str.rbegin ();
    for (; rit != str.rend (); ++rit)
    {
        if (! is_space (*rit))
            break;
    }
    str.erase (rit.base (), str.end ());
}


static
void
trim_ws (tstring & str)
{
    trim_trailing_ws (str);
    trim_leading_ws (str);
}


#if defined (LOG4CPLUS_WITH_UNIT_TESTS)
CATCH_TEST_CASE( "String trimming", "[strings][properties]")
{
    CATCH_SECTION ("trim trailing whitespace")
    {
        tstring trailing_ws (LOG4CPLUS_TEXT ("abcd \t\n\v\f\r"));
        trim_trailing_ws (trailing_ws);
        CATCH_REQUIRE (trailing_ws == LOG4CPLUS_TEXT ("abcd"));
    }

    CATCH_SECTION ("trim leading whitespace")
    {
        tstring leading_ws (LOG4CPLUS_TEXT (" \t\n\v\f\rabcd"));
        trim_leading_ws (leading_ws);
        CATCH_REQUIRE (leading_ws == LOG4CPLUS_TEXT ("abcd"));
    }

    CATCH_SECTION ("trim all whitespace")
    {
        tstring ws (LOG4CPLUS_TEXT (" \t\n\v\f\rabcd \t\n\v\f\r"));
        trim_ws (ws);
        CATCH_REQUIRE (ws == LOG4CPLUS_TEXT ("abcd"));
    }
}
#endif


void
imbue_file_from_flags ([[maybe_unused]] tistream & file, unsigned flags)
{
    switch (flags & (Properties::fEncodingMask << Properties::fEncodingShift))
    {
#if defined (LOG4CPLUS_HAVE_CODECVT_UTF8_FACET) && defined (UNICODE)
    case Properties::fUTF8:
        file.imbue (
            std::locale (file.getloc (),
                new std::codecvt_utf8<tchar, 0x10FFFF,
                    static_cast<std::codecvt_mode>(std::consume_header | std::little_endian)>));
        break;
#endif

#if defined (LOG4CPLUS_HAVE_CODECVT_UTF16_FACET) && defined (UNICODE)
    case Properties::fUTF16:
        file.imbue (
            std::locale (file.getloc (),
                new std::codecvt_utf16<tchar, 0x10FFFF,
                    static_cast<std::codecvt_mode>(std::consume_header | std::little_endian)>));
        break;

#elif defined (UNICODE) && defined (_WIN32)
    case Properties::fUTF16:
        file.imbue (
            std::locale (file.getloc (),
                // TODO: This should actually be a custom "null" facet
                // that just copies the chars to wchar_t buffer.
                new std::codecvt<wchar_t, char, std::mbstate_t>));
        break;

#endif

#if defined (LOG4CPLUS_HAVE_CODECVT_UTF32_FACET) && defined (UNICODE)
    case Properties::fUTF32:
        file.imbue (
            std::locale (file.getloc (),
                new std::codecvt_utf32<tchar, 0x10FFFF,
                    static_cast<std::codecvt_mode>(std::consume_header | std::little_endian)>));
        break;
#endif

    case Properties::fUnspecEncoding:;
    default:
        // Do nothing.
        ;
    }
}


} // namespace


/**
 * Perform variable substitution in string <code>val</code> from
 * environment variables.
 *
 * <p>The variable substitution delimeters are <b>${</b> and <b>}</b>.
 *
 * <p>For example, if the System properties contains "key=value", then
 * the call
 * <pre>
 * string s;
 * substEnvironVars(s, "Value of key is ${key}.");
 * </pre>
 *
 * will set the variable <code>s</code> to "Value of key is value.".
 *
 * <p>If no value could be found for the specified key, then
 * substitution defaults to the empty string.
 *
 * <p>For example, if there is no environment variable "inexistentKey",
 * then the call
 *
 * <pre>
 * string s;
 * substEnvironVars(s, "Value of inexistentKey is [${inexistentKey}]");
 * </pre>
 * will set <code>s</code> to "Value of inexistentKey is []"
 *
 * @param val The string on which variable substitution is performed.
 * @param dest The result.
 */
bool
substVars (tstring & dest, const tstring & val,
    helpers::Properties const & props, helpers::LogLog& loglog,
    unsigned flags)
{
    tchar constexpr DELIM_START[] = LOG4CPLUS_TEXT("${");
    tchar constexpr DELIM_STOP[] = LOG4CPLUS_TEXT("}");
    std::size_t constexpr DELIM_START_LEN = 2;
    std::size_t constexpr DELIM_STOP_LEN = 1;

    tstring::size_type i = 0;
    tstring::size_type var_start, var_end;
    tstring pattern (val);
    tstring key;
    tstring replacement;
    bool changed = false;
    bool const empty_vars
        = !! (flags & PropertyConfigurator::fAllowEmptyVars);
    bool const shadow_env
        = !! (flags & PropertyConfigurator::fShadowEnvironment);
    bool const rec_exp
        = !! (flags & PropertyConfigurator::fRecursiveExpansion);

    while (true)
    {
        // Find opening paren of variable substitution.
        var_start = pattern.find(DELIM_START, i);
        if (var_start == tstring::npos)
        {
            dest = pattern;
            return changed;
        }

        // Find closing paren of variable substitution.
        var_end = pattern.find(DELIM_STOP, var_start);
        if (var_end == tstring::npos)
        {
            tostringstream buffer;
            buffer << '"' << pattern
                    << "\" has no closing brace. "
                    << "Opening brace at position " << var_start << ".";
            loglog.error(buffer.str());
            dest = val;
            return false;
        }

        key.assign (pattern, var_start + DELIM_START_LEN,
            var_end - (var_start + DELIM_START_LEN));
        replacement.clear ();
        if (shadow_env)
            replacement = props.getProperty (key);
        if (! shadow_env || (! empty_vars && replacement.empty ()))
            internal::get_env_var (replacement, key);

        if (empty_vars || ! replacement.empty ())
        {
            // Substitute the variable with its value in place.
            pattern.replace (var_start, var_end - var_start + DELIM_STOP_LEN,
                replacement);
            changed = true;
            if (rec_exp)
                // Retry expansion on the same spot.
                continue;
            else
                // Move beyond the just substituted part.
                i = var_start + replacement.size ();
        }
        else
            // Nothing has been subtituted, just move beyond the
            // unexpanded variable.
            i = var_end + DELIM_STOP_LEN;
    } // end while loop

} // end substVars()


///////////////////////////////////////////////////////////////////////////////
// Properties ctors and dtor
///////////////////////////////////////////////////////////////////////////////

Properties::Properties()
    : flags (0)
{
}



Properties::Properties(tistream& input)
    : flags (0)
{
    init(input);
}



Properties::Properties(const tstring& inputFile, unsigned f)
    : flags (f)
{
    if (inputFile.empty ())
        return;

    tifstream file;
    imbue_file_from_flags (file, flags);

    file.open(std::filesystem::path (inputFile),
        std::ios::binary);
    if (! file.good ())
        helpers::getLogLog ().error (LOG4CPLUS_TEXT ("could not open file ")
            + inputFile, (flags & Properties::fThrow) != 0);

    init(file);
}



void
Properties::init(tistream& input)
{
    if (! input)
        return;

    tstring buffer;
    while (std::getline (input, buffer))
    {
        trim_leading_ws (buffer);

        tstring::size_type const buffLen = buffer.size ();
        if (buffLen == 0 || buffer[0] == PROPERTIES_COMMENT_CHAR)
            continue;

        // Check if we have a trailing \r because we are
        // reading a properties file produced on Windows.
        if (buffer[buffLen-1] == LOG4CPLUS_TEXT('\r'))
            // Remove trailing 'Windows' \r.
            buffer.resize (buffLen - 1);

        if (buffer.size () >= 7 + 1 + 1
            && buffer.compare (0, 7, LOG4CPLUS_TEXT ("include")) == 0
            && is_space (buffer[7]))
        {
            tstring included (buffer, 8) ;
            trim_ws (included);

            tstring subIncluded;
            helpers::substVars(subIncluded, included, *this, helpers::getLogLog(), 0);

            tifstream file;
            imbue_file_from_flags (file, flags);

            file.open (std::filesystem::path (subIncluded),
                std::ios::binary);
            if (! file.good ())
                helpers::getLogLog ().error (
                    LOG4CPLUS_TEXT ("could not open file ") + subIncluded);

            init (file);
        }
        else
        {
            if (auto const idx = buffer.find(LOG4CPLUS_TEXT ('=')); idx != tstring::npos)
            {
                tstring key = buffer.substr(0, idx);
                tstring value = buffer.substr(idx + 1);
                trim_trailing_ws (key);
                trim_ws (value);
                setProperty(key, value);
            }
        }
    }
}



Properties::~Properties() = default;



///////////////////////////////////////////////////////////////////////////////
// helpers::Properties public methods
///////////////////////////////////////////////////////////////////////////////


bool
Properties::exists(const log4cplus::tstring& key) const
{
    return data.find(key) != data.end();
}


bool
Properties::exists(tchar const * key) const
{
    return data.find(key) != data.end();
}


tstring const &
Properties::getProperty(const tstring& key) const
{
    return get_property_worker (key);
}


log4cplus::tstring const &
Properties::getProperty(tchar const * key) const
{
    return get_property_worker (key);
}


tstring
Properties::getProperty(const tstring& key, const tstring& defaultVal) const
{
    if (auto it {data.find (key)}; it == data.end ())
        return defaultVal;
    else
        return it->second;
}


std::vector<tstring>
Properties::propertyNames() const
{
    std::vector<tstring> tmp;
    tmp.reserve (data.size ());
    for (auto const & kv : data)
        tmp.push_back(kv.first);

    return tmp;
}



void
Properties::setProperty(const log4cplus::tstring& key,
    const log4cplus::tstring& value)
{
    data[key] = value;
}


bool
Properties::removeProperty(const log4cplus::tstring& key)
{
    return data.erase(key) > 0;
}


Properties
Properties::getPropertySubset(const log4cplus::tstring& prefix) const
{
    Properties ret;
    auto const prefix_len = prefix.size ();
    std::vector<tstring> keys = propertyNames();
    for (tstring const & key : keys)
    {
        int result = key.compare (0, prefix_len, prefix);
        if (result == 0)
            ret.setProperty (key.substr (prefix_len), getProperty(key));
    }

    return ret;
}


bool
Properties::getInt (int & val, log4cplus::tstring const & key) const
{
    return get_type_val_worker (val, key);
}


bool
Properties::getUInt (unsigned & val, log4cplus::tstring const & key) const
{
    return get_type_val_worker (val, key);
}


bool
Properties::getLong (long & val, log4cplus::tstring const & key) const
{
    return get_type_val_worker (val, key);
}


bool
Properties::getULong (unsigned long & val, log4cplus::tstring const & key) const
{
    return get_type_val_worker (val, key);
}


bool
Properties::getBool (bool & val, log4cplus::tstring const & key) const
{
    if (! exists (key))
        return false;

    log4cplus::tstring const & prop_val = getProperty (key);
    return internal::parse_bool (val, prop_val);
}


bool
Properties::getString (log4cplus::tstring & val, log4cplus::tstring const & key)
    const
{
    auto it (data.find (key));
    if (it == data.end ())
        return false;

    val = it->second;
    return true;
}


template <typename StringType>
log4cplus::tstring const &
Properties::get_property_worker (StringType const & key) const
{
    if (auto it {data.find (key)}; it == data.end ())
        return log4cplus::internal::empty_str;
    else
        return it->second;
}


template <typename ValType>
bool
Properties::get_type_val_worker (ValType & val, log4cplus::tstring const & key)
    const
{
    if (! exists (key))
        return false;

    log4cplus::tstring const & prop_val = getProperty (key);
    log4cplus::tistringstream iss (prop_val);
    ValType tmp_val;
    tchar ch;

    if (iss >> tmp_val; ! iss)
        return false;
    if (iss >> ch; iss)
        return false;

    val = tmp_val;
    return true;
}


#if defined (LOG4CPLUS_WITH_UNIT_TESTS)
CATCH_TEST_CASE ("Properties", "[properties]")
{
    static tchar const PROP_ABC[] = LOG4CPLUS_TEXT ("a.b.c");
    Properties props;

    CATCH_SECTION ("new object is empty")
    {
        CATCH_REQUIRE (props.size () == 0);
    }

    CATCH_SECTION ("added property can be retrieved")
    {
        props.setProperty (PROP_ABC, LOG4CPLUS_TEXT ("true"));
        CATCH_REQUIRE (props.exists (PROP_ABC));
        CATCH_REQUIRE (props.exists (LOG4CPLUS_C_STR_TO_TSTRING (PROP_ABC)));
        CATCH_REQUIRE (props.getProperty (PROP_ABC)
            == LOG4CPLUS_TEXT ("true"));
    }

    CATCH_SECTION ("type conversions work")
    {
        bool bool_;
        int int_;
        unsigned int uint;
        long long_;
        unsigned long ulong;

        tistringstream iss (
            LOG4CPLUS_TEXT ("bool=true\r\n")
            LOG4CPLUS_TEXT ("bool1=1\n")
            LOG4CPLUS_TEXT ("int=-1\n")
            LOG4CPLUS_TEXT ("uint=42\n")
            LOG4CPLUS_TEXT ("long=-65537\n")
            LOG4CPLUS_TEXT ("ulong=65537")
        );
        Properties from_stream (iss);

        CATCH_REQUIRE (from_stream.getBool (bool_, LOG4CPLUS_TEXT ("bool")));
        CATCH_REQUIRE (bool_);
        CATCH_REQUIRE (from_stream.getBool (bool_, LOG4CPLUS_TEXT ("bool1")));
        CATCH_REQUIRE (bool_);
        CATCH_REQUIRE (from_stream.getInt (int_, LOG4CPLUS_TEXT ("int")));
        CATCH_REQUIRE (int_ == -1);
        CATCH_REQUIRE (from_stream.getUInt (uint, LOG4CPLUS_TEXT ("uint")));
        CATCH_REQUIRE (uint == 42);
        CATCH_REQUIRE (from_stream.getLong (long_, LOG4CPLUS_TEXT ("long")));
        CATCH_REQUIRE (long_ == -65537);
        CATCH_REQUIRE (from_stream.getULong (ulong, LOG4CPLUS_TEXT ("ulong")));
        CATCH_REQUIRE (ulong == 65537);
    }

    CATCH_SECTION ("remove property")
    {
        props.setProperty (PROP_ABC, LOG4CPLUS_TEXT ("true"));
        CATCH_REQUIRE (props.exists (PROP_ABC));
        props.removeProperty (PROP_ABC);
        CATCH_REQUIRE (! props.exists (PROP_ABC));
    }

    CATCH_SECTION ("retrieve property names")
    {
        props.setProperty (PROP_ABC, LOG4CPLUS_TEXT ("true"));
        static tchar const PROP_SECOND[] = LOG4CPLUS_TEXT ("second");
        props.setProperty (LOG4CPLUS_TEXT ("second"), LOG4CPLUS_TEXT ("false"));
        std::vector<log4cplus::tstring> names (props.propertyNames ());
        CATCH_REQUIRE (std::find (std::begin (names), std::end (names),
                PROP_ABC) != std::end (names));
        CATCH_REQUIRE (std::find (std::begin (names), std::end (names),
                PROP_SECOND) != std::end (names));
    }

    CATCH_SECTION ("throw on nonexistent file")
    {
        auto && f = [] {
            Properties props2 (LOG4CPLUS_TEXT ("xxx does not exist"), Properties::fThrow);
        };
        CATCH_REQUIRE_THROWS_AS (f (), log4cplus::exception);
    }
}
#endif


} // namespace log4cplus::helpers

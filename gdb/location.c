/* Data structures and API for event locations in GDB.
   Copyright (C) 2013-2022 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "gdbsupport/gdb_assert.h"
#include "location.h"
#include "symtab.h"
#include "language.h"
#include "linespec.h"
#include "cli/cli-utils.h"
#include "probe.h"
#include "cp-support.h"

#include <ctype.h>
#include <string.h>

static std::string explicit_location_to_string
     (const struct explicit_location *explicit_loc);

/* The base class for all an event locations used to set a stop event
   in the inferior.  */

struct event_location
{
  virtual ~event_location () = default;

  /* Clone this object.  */
  virtual event_location_up clone () const = 0;

  /* Return true if this location is empty, false otherwise.  */
  virtual bool empty_p () const = 0;

  /* Return a string representation of this location.  */
  const char *to_string () const
  {
    if (as_string.empty ())
      as_string = compute_string ();
    if (as_string.empty ())
      return nullptr;
    return as_string.c_str ();
  }

  DISABLE_COPY_AND_ASSIGN (event_location);

  /* The type of this breakpoint specification.  */
  enum event_location_type type;

  /* Cached string representation of this location.  This is used,
     e.g., to save stop event locations to file.  */
  mutable std::string as_string;

protected:

  explicit event_location (enum event_location_type t)
    : type (t)
  {
  }

  event_location (enum event_location_type t, std::string &&str)
    : type (t),
      as_string (std::move (str))
  {
  }

  explicit event_location (const event_location *to_clone)
    : type (to_clone->type),
      as_string (to_clone->as_string)
  {
  }

  /* Compute the string representation of this object.  This is called
     by to_string when needed.  */
  virtual std::string compute_string () const = 0;
};

/* A probe.  */
struct event_location_probe : public event_location
{
  explicit event_location_probe (std::string &&probe)
    : event_location (PROBE_LOCATION, std::move (probe))
  {
  }

  event_location_up clone () const override
  {
    return event_location_up (new event_location_probe (this));
  }

  bool empty_p () const override
  {
    return false;
  }

protected:

  explicit event_location_probe (const event_location_probe *to_clone)
    : event_location (to_clone)
  {
  }

  std::string compute_string () const override
  {
    return std::move (as_string);
  }
};

/* A "normal" linespec.  */
struct event_location_linespec : public event_location
{
  event_location_linespec (const char **linespec,
			   symbol_name_match_type match_type)
    : event_location (LINESPEC_LOCATION)
  {
    linespec_location.match_type = match_type;
    if (*linespec != NULL)
      {
	const char *p;
	const char *orig = *linespec;

	linespec_lex_to_end (linespec);
	p = remove_trailing_whitespace (orig, *linespec);
	if ((p - orig) > 0)
	  linespec_location.spec_string = savestring (orig, p - orig);
      }
  }

  ~event_location_linespec ()
  {
    xfree (linespec_location.spec_string);
  }

  event_location_up clone () const override
  {
    return event_location_up (new event_location_linespec (this));
  }

  bool empty_p () const override
  {
    return false;
  }

  struct linespec_location linespec_location {};

protected:

  explicit event_location_linespec (const event_location_linespec *to_clone)
    : event_location (to_clone),
      linespec_location (to_clone->linespec_location)
  {
    if (linespec_location.spec_string != nullptr)
      linespec_location.spec_string = xstrdup (linespec_location.spec_string);
  }

  std::string compute_string () const override
  {
    if (linespec_location.spec_string != nullptr)
      {
	const struct linespec_location *ls = &linespec_location;
	if (ls->match_type == symbol_name_match_type::FULL)
	  return std::string ("-qualified ") + ls->spec_string;
	else
	  return ls->spec_string;
      }
    return {};
  }
};

/* An address in the inferior.  */
struct event_location_address : public event_location
{
  event_location_address (CORE_ADDR addr, const char *addr_string,
			  int addr_string_len)
    : event_location (ADDRESS_LOCATION),
      address (addr)
  {
    if (addr_string != nullptr)
      as_string = std::string (addr_string, addr_string_len);
  }

  event_location_up clone () const override
  {
    return event_location_up (new event_location_address (this));
  }

  bool empty_p () const override
  {
    return false;
  }

  CORE_ADDR address;

protected:

  event_location_address (const event_location_address *to_clone)
    : event_location (to_clone),
      address (to_clone->address)
  {
  }

  std::string compute_string () const override
  {
    const char *addr_string = core_addr_to_string (address);
    return std::string ("*") + addr_string;
  }
};

/* An explicit location.  */
struct event_location_explicit : public event_location
{
  explicit event_location_explicit (const struct explicit_location *loc)
    : event_location (EXPLICIT_LOCATION)
  {
    copy_loc (loc);
  }

  ~event_location_explicit ()
  {
    xfree (explicit_loc.source_filename);
    xfree (explicit_loc.function_name);
    xfree (explicit_loc.label_name);
  }

  event_location_up clone () const override
  {
    return event_location_up (new event_location_explicit (this));
  }

  bool empty_p () const override
  {
    return (explicit_loc.source_filename == nullptr
	    && explicit_loc.function_name == nullptr
	    && explicit_loc.label_name == nullptr
	    && explicit_loc.line_offset.sign == LINE_OFFSET_UNKNOWN);
  }

  struct explicit_location explicit_loc;

protected:

  explicit event_location_explicit (const event_location_explicit *to_clone)
    : event_location (to_clone)
  {
    copy_loc (&to_clone->explicit_loc);
  }

  std::string compute_string () const override
  {
    return explicit_location_to_string (&explicit_loc);
  }

private:

  void copy_loc (const struct explicit_location *loc)
  {
    initialize_explicit_location (&explicit_loc);
    if (loc != nullptr)
      {
	explicit_loc.func_name_match_type = loc->func_name_match_type;
	if (loc->source_filename != nullptr)
	  explicit_loc.source_filename = xstrdup (loc->source_filename);
	if (loc->function_name != nullptr)
	  explicit_loc.function_name = xstrdup (loc->function_name);
	if (loc->label_name != nullptr)
	  explicit_loc.label_name = xstrdup (loc->label_name);
	explicit_loc.line_offset = loc->line_offset;
      }
  }
};

/* See description in location.h.  */

enum event_location_type
event_location_type (const struct event_location *location)
{
  return location->type;
}

/* See description in location.h.  */

void
initialize_explicit_location (struct explicit_location *explicit_loc)
{
  memset (explicit_loc, 0, sizeof (struct explicit_location));
  explicit_loc->line_offset.sign = LINE_OFFSET_UNKNOWN;
  explicit_loc->func_name_match_type = symbol_name_match_type::WILD;
}

/* See description in location.h.  */

event_location_up
new_linespec_location (const char **linespec,
		       symbol_name_match_type match_type)
{
  return event_location_up (new event_location_linespec (linespec,
							 match_type));
}

/* See description in location.h.  */

const linespec_location *
get_linespec_location (const struct event_location *location)
{
  gdb_assert (location->type == LINESPEC_LOCATION);
  return &((event_location_linespec *) location)->linespec_location;
}

/* See description in location.h.  */

event_location_up
new_address_location (CORE_ADDR addr, const char *addr_string,
		      int addr_string_len)
{
  return event_location_up (new event_location_address (addr, addr_string,
							addr_string_len));
}

/* See description in location.h.  */

CORE_ADDR
get_address_location (const struct event_location *location)
{
  gdb_assert (location->type == ADDRESS_LOCATION);
  return ((event_location_address *) location)->address;
}

/* See description in location.h.  */

const char *
get_address_string_location (const struct event_location *location)
{
  gdb_assert (location->type == ADDRESS_LOCATION);
  return location->to_string ();
}

/* See description in location.h.  */

event_location_up
new_probe_location (std::string &&probe)
{
  return event_location_up (new event_location_probe (std::move (probe)));
}

/* See description in location.h.  */

const char *
get_probe_location (const struct event_location *location)
{
  gdb_assert (location->type == PROBE_LOCATION);
  return location->to_string ();
}

/* See description in location.h.  */

event_location_up
new_explicit_location (const struct explicit_location *explicit_loc)
{
  return event_location_up (new event_location_explicit (explicit_loc));
}

/* See description in location.h.  */

struct explicit_location *
get_explicit_location (struct event_location *location)
{
  gdb_assert (location->type == EXPLICIT_LOCATION);
  return &((event_location_explicit *) location)->explicit_loc;
}

/* See description in location.h.  */

const struct explicit_location *
get_explicit_location_const (const struct event_location *location)
{
  gdb_assert (location->type == EXPLICIT_LOCATION);
  return &((event_location_explicit *) location)->explicit_loc;
}

/* This convenience function returns a malloc'd string which
   represents the location in EXPLICIT_LOC.

   AS_LINESPEC is true if this string should be a linespec.
   Otherwise it will be output in explicit form.  */

static std::string
explicit_to_string_internal (bool as_linespec,
			     const struct explicit_location *explicit_loc)
{
  bool need_space = false;
  char space = as_linespec ? ':' : ' ';
  string_file buf;

  if (explicit_loc->source_filename != NULL)
    {
      if (!as_linespec)
	buf.puts ("-source ");
      buf.puts (explicit_loc->source_filename);
      need_space = true;
    }

  if (explicit_loc->function_name != NULL)
    {
      if (need_space)
	buf.putc (space);
      if (explicit_loc->func_name_match_type == symbol_name_match_type::FULL)
	buf.puts ("-qualified ");
      if (!as_linespec)
	buf.puts ("-function ");
      buf.puts (explicit_loc->function_name);
      need_space = true;
    }

  if (explicit_loc->label_name != NULL)
    {
      if (need_space)
	buf.putc (space);
      if (!as_linespec)
	buf.puts ("-label ");
      buf.puts (explicit_loc->label_name);
      need_space = true;
    }

  if (explicit_loc->line_offset.sign != LINE_OFFSET_UNKNOWN)
    {
      if (need_space)
	buf.putc (space);
      if (!as_linespec)
	buf.puts ("-line ");
      buf.printf ("%s%d",
		  (explicit_loc->line_offset.sign == LINE_OFFSET_NONE ? ""
		   : (explicit_loc->line_offset.sign
		      == LINE_OFFSET_PLUS ? "+" : "-")),
		  explicit_loc->line_offset.offset);
    }

  return buf.release ();
}

/* See description in location.h.  */

static std::string
explicit_location_to_string (const struct explicit_location *explicit_loc)
{
  return explicit_to_string_internal (false, explicit_loc);
}

/* See description in location.h.  */

std::string
explicit_location_to_linespec (const struct explicit_location *explicit_loc)
{
  return explicit_to_string_internal (true, explicit_loc);
}

/* See description in location.h.  */

event_location_up
copy_event_location (const struct event_location *src)
{
  return src->clone ();
}

void
event_location_deleter::operator() (event_location *location) const
{
  delete location;
}

/* See description in location.h.  */

const char *
event_location_to_string (struct event_location *location)
{
  return location->to_string ();
}

/* Find an instance of the quote character C in the string S that is
   outside of all single- and double-quoted strings (i.e., any quoting
   other than C).  */

static const char *
find_end_quote (const char *s, char end_quote_char)
{
  /* zero if we're not in quotes;
     '"' if we're in a double-quoted string;
     '\'' if we're in a single-quoted string.  */
  char nested_quote_char = '\0';

  for (const char *scan = s; *scan != '\0'; scan++)
    {
      if (nested_quote_char != '\0')
	{
	  if (*scan == nested_quote_char)
	    nested_quote_char = '\0';
	  else if (scan[0] == '\\' && *(scan + 1) != '\0')
	    scan++;
	}
      else if (*scan == end_quote_char && nested_quote_char == '\0')
	return scan;
      else if (*scan == '"' || *scan == '\'')
	nested_quote_char = *scan;
    }

  return 0;
}

/* A lexer for explicit locations.  This function will advance INP
   past any strings that it lexes.  Returns a malloc'd copy of the
   lexed string or NULL if no lexing was done.  */

static gdb::unique_xmalloc_ptr<char>
explicit_location_lex_one (const char **inp,
			   const struct language_defn *language,
			   explicit_completion_info *completion_info)
{
  const char *start = *inp;

  if (*start == '\0')
    return NULL;

  /* If quoted, skip to the ending quote.  */
  if (strchr (get_gdb_linespec_parser_quote_characters (), *start))
    {
      if (completion_info != NULL)
	completion_info->quoted_arg_start = start;

      const char *end = find_end_quote (start + 1, *start);

      if (end == NULL)
	{
	  if (completion_info == NULL)
	    error (_("Unmatched quote, %s."), start);

	  end = start + strlen (start);
	  *inp = end;
	  return gdb::unique_xmalloc_ptr<char> (savestring (start + 1,
							    *inp - start - 1));
	}

      if (completion_info != NULL)
	completion_info->quoted_arg_end = end;
      *inp = end + 1;
      return gdb::unique_xmalloc_ptr<char> (savestring (start + 1,
							*inp - start - 2));
    }

  /* If the input starts with '-' or '+', the string ends with the next
     whitespace or comma.  */
  if (*start == '-' || *start == '+')
    {
      while (*inp[0] != '\0' && *inp[0] != ',' && !isspace (*inp[0]))
	++(*inp);
    }
  else
    {
      /* Handle numbers first, stopping at the next whitespace or ','.  */
      while (isdigit (*inp[0]))
	++(*inp);
      if (*inp[0] == '\0' || isspace (*inp[0]) || *inp[0] == ',')
	return gdb::unique_xmalloc_ptr<char> (savestring (start,
							  *inp - start));

      /* Otherwise stop at the next occurrence of whitespace, '\0',
	 keyword, or ','.  */
      *inp = start;
      while ((*inp)[0]
	     && (*inp)[0] != ','
	     && !(isspace ((*inp)[0])
		  || linespec_lexer_lex_keyword (&(*inp)[1])))
	{
	  /* Special case: C++ operator,.  */
	  if (language->la_language == language_cplus
	      && startswith (*inp, CP_OPERATOR_STR))
	    (*inp) += CP_OPERATOR_LEN;
	  ++(*inp);
	}
    }

  if (*inp - start > 0)
    return gdb::unique_xmalloc_ptr<char> (savestring (start, *inp - start));

  return NULL;
}

/* Return true if COMMA points past "operator".  START is the start of
   the line that COMMAND points to, hence when reading backwards, we
   must not read any character before START.  */

static bool
is_cp_operator (const char *start, const char *comma)
{
  if (comma != NULL
      && (comma - start) >= CP_OPERATOR_LEN)
    {
      const char *p = comma;

      while (p > start && isspace (p[-1]))
	p--;
      if (p - start >= CP_OPERATOR_LEN)
	{
	  p -= CP_OPERATOR_LEN;
	  if (strncmp (p, CP_OPERATOR_STR, CP_OPERATOR_LEN) == 0
	      && (p == start
		  || !(isalnum (p[-1]) || p[-1] == '_')))
	    {
	      return true;
	    }
	}
    }
  return false;
}

/* When scanning the input string looking for the next explicit
   location option/delimiter, we jump to the next option by looking
   for ",", and "-".  Such a character can also appear in C++ symbols
   like "operator," and "operator-".  So when we find such a
   character, we call this function to check if we found such a
   symbol, meaning we had a false positive for an option string.  In
   that case, we keep looking for the next delimiter, until we find
   one that is not a false positive, or we reach end of string.  FOUND
   is the character that scanning found (either '-' or ','), and START
   is the start of the line that FOUND points to, hence when reading
   backwards, we must not read any character before START.  Returns a
   pointer to the next non-false-positive delimiter character, or NULL
   if none was found.  */

static const char *
skip_op_false_positives (const char *start, const char *found)
{
  while (found != NULL && is_cp_operator (start, found))
    {
      if (found[0] == '-' && found[1] == '-')
	start = found + 2;
      else
	start = found + 1;
      found = find_toplevel_char (start, *found);
    }

  return found;
}

/* Assuming both FIRST and NEW_TOK point into the same string, return
   the pointer that is closer to the start of the string.  If FIRST is
   NULL, returns NEW_TOK.  If NEW_TOK is NULL, returns FIRST.  */

static const char *
first_of (const char *first, const char *new_tok)
{
  if (first == NULL)
    return new_tok;
  else if (new_tok != NULL && new_tok < first)
    return new_tok;
  else
    return first;
}

/* A lexer for functions in explicit locations.  This function will
   advance INP past a function until the next option, or until end of
   string.  Returns a malloc'd copy of the lexed string or NULL if no
   lexing was done.  */

static gdb::unique_xmalloc_ptr<char>
explicit_location_lex_one_function (const char **inp,
				    const struct language_defn *language,
				    explicit_completion_info *completion_info)
{
  const char *start = *inp;

  if (*start == '\0')
    return NULL;

  /* If quoted, skip to the ending quote.  */
  if (strchr (get_gdb_linespec_parser_quote_characters (), *start))
    {
      char quote_char = *start;

      /* If the input is not an Ada operator, skip to the matching
	 closing quote and return the string.  */
      if (!(language->la_language == language_ada
	    && quote_char == '\"' && is_ada_operator (start)))
	{
	  if (completion_info != NULL)
	    completion_info->quoted_arg_start = start;

	  const char *end = find_toplevel_char (start + 1, quote_char);

	  if (end == NULL)
	    {
	      if (completion_info == NULL)
		error (_("Unmatched quote, %s."), start);

	      end = start + strlen (start);
	      *inp = end;
	      char *saved = savestring (start + 1, *inp - start - 1);
	      return gdb::unique_xmalloc_ptr<char> (saved);
	    }

	  if (completion_info != NULL)
	    completion_info->quoted_arg_end = end;
	  *inp = end + 1;
	  char *saved = savestring (start + 1, *inp - start - 2);
	  return gdb::unique_xmalloc_ptr<char> (saved);
	}
    }

  const char *comma = find_toplevel_char (start, ',');

  /* If we have "-function -myfunction", or perhaps better example,
     "-function -[BasicClass doIt]" (objc selector), treat
     "-myfunction" as the function name.  I.e., skip the first char if
     it is an hyphen.  Don't skip the first char always, because we
     may have C++ "operator<", and find_toplevel_char needs to see the
     'o' in that case.  */
  const char *hyphen
    = (*start == '-'
       ? find_toplevel_char (start + 1, '-')
       : find_toplevel_char (start, '-'));

  /* Check for C++ "operator," and "operator-".  */
  comma = skip_op_false_positives (start, comma);
  hyphen = skip_op_false_positives (start, hyphen);

  /* Pick the one that appears first.  */
  const char *end = first_of (hyphen, comma);

  /* See if a linespec keyword appears first.  */
  const char *s = start;
  const char *ws = find_toplevel_char (start, ' ');
  while (ws != NULL && linespec_lexer_lex_keyword (ws + 1) == NULL)
    {
      s = ws + 1;
      ws = find_toplevel_char (s, ' ');
    }
  if (ws != NULL)
    end = first_of (end, ws + 1);

  /* If we don't have any terminator, then take the whole string.  */
  if (end == NULL)
    end = start + strlen (start);

  /* Trim whitespace at the end.  */
  while (end > start && end[-1] == ' ')
    end--;

  *inp = end;

  if (*inp - start > 0)
    return gdb::unique_xmalloc_ptr<char> (savestring (start, *inp - start));

  return NULL;
}

/* See description in location.h.  */

event_location_up
string_to_explicit_location (const char **argp,
			     const struct language_defn *language,
			     explicit_completion_info *completion_info)
{
  /* It is assumed that input beginning with '-' and a non-digit
     character is an explicit location.  "-p" is reserved, though,
     for probe locations.  */
  if (argp == NULL
      || *argp == NULL
      || *argp[0] != '-'
      || !isalpha ((*argp)[1])
      || ((*argp)[0] == '-' && (*argp)[1] == 'p'))
    return NULL;

  std::unique_ptr<event_location_explicit> location
    (new event_location_explicit ((const explicit_location *) nullptr));

  /* Process option/argument pairs.  dprintf_command
     requires that processing stop on ','.  */
  while ((*argp)[0] != '\0' && (*argp)[0] != ',')
    {
      int len;
      const char *start;

      /* Clear these on each iteration, since they should be filled
	 with info about the last option.  */
      if (completion_info != NULL)
	{
	  completion_info->quoted_arg_start = NULL;
	  completion_info->quoted_arg_end = NULL;
	}

      /* If *ARGP starts with a keyword, stop processing
	 options.  */
      if (linespec_lexer_lex_keyword (*argp) != NULL)
	break;

      /* Mark the start of the string in case we need to rewind.  */
      start = *argp;

      if (completion_info != NULL)
	completion_info->last_option = start;

      /* Get the option string.  */
      gdb::unique_xmalloc_ptr<char> opt
	= explicit_location_lex_one (argp, language, NULL);

      /* Use the length of the option to allow abbreviations.  */
      len = strlen (opt.get ());

      /* Get the argument string.  */
      *argp = skip_spaces (*argp);

      /* All options have a required argument.  Checking for this
	 required argument is deferred until later.  */
      gdb::unique_xmalloc_ptr<char> oarg;
      /* True if we have an argument.  This is required because we'll
	 move from OARG before checking whether we have an
	 argument.  */
      bool have_oarg = false;

      /* True if the option needs an argument.  */
      bool need_oarg = false;

      /* Convenience to consistently set both OARG/HAVE_OARG from
	 ARG.  */
      auto set_oarg = [&] (gdb::unique_xmalloc_ptr<char> arg)
	{
	  if (completion_info != NULL)
	    {
	      /* We do this here because the set of options that take
		 arguments matches the set of explicit location
		 options.  */
	      completion_info->saw_explicit_location_option = true;
	    }
	  oarg = std::move (arg);
	  have_oarg = oarg != NULL;
	  need_oarg = true;
	};

      if (strncmp (opt.get (), "-source", len) == 0)
	{
	  set_oarg (explicit_location_lex_one (argp, language,
					       completion_info));
	  location->explicit_loc.source_filename = oarg.release ();
	}
      else if (strncmp (opt.get (), "-function", len) == 0)
	{
	  set_oarg (explicit_location_lex_one_function (argp, language,
							completion_info));
	  location->explicit_loc.function_name = oarg.release ();
	}
      else if (strncmp (opt.get (), "-qualified", len) == 0)
	{
	  location->explicit_loc.func_name_match_type
	    = symbol_name_match_type::FULL;
	}
      else if (strncmp (opt.get (), "-line", len) == 0)
	{
	  set_oarg (explicit_location_lex_one (argp, language, NULL));
	  *argp = skip_spaces (*argp);
	  if (have_oarg)
	    {
	      location->explicit_loc.line_offset
		= linespec_parse_line_offset (oarg.get ());
	      continue;
	    }
	}
      else if (strncmp (opt.get (), "-label", len) == 0)
	{
	  set_oarg (explicit_location_lex_one (argp, language, completion_info));
	  location->explicit_loc.label_name = oarg.release ();
	}
      /* Only emit an "invalid argument" error for options
	 that look like option strings.  */
      else if (opt.get ()[0] == '-' && !isdigit (opt.get ()[1]))
	{
	  if (completion_info == NULL)
	    error (_("invalid explicit location argument, \"%s\""), opt.get ());
	}
      else
	{
	  /* End of the explicit location specification.
	     Stop parsing and return whatever explicit location was
	     parsed.  */
	  *argp = start;
	  break;
	}

      *argp = skip_spaces (*argp);

      /* It's a little lame to error after the fact, but in this
	 case, it provides a much better user experience to issue
	 the "invalid argument" error before any missing
	 argument error.  */
      if (need_oarg && !have_oarg && completion_info == NULL)
	error (_("missing argument for \"%s\""), opt.get ());
    }

  /* One special error check:  If a source filename was given
     without offset, function, or label, issue an error.  */
  if (location->explicit_loc.source_filename != NULL
      && location->explicit_loc.function_name == NULL
      && location->explicit_loc.label_name == NULL
      && (location->explicit_loc.line_offset.sign == LINE_OFFSET_UNKNOWN)
      && completion_info == NULL)
    {
      error (_("Source filename requires function, label, or "
	       "line offset."));
    }

  return event_location_up (location.release ());
}

/* See description in location.h.  */

event_location_up
string_to_event_location_basic (const char **stringp,
				const struct language_defn *language,
				symbol_name_match_type match_type)
{
  event_location_up location;
  const char *cs;

  /* Try the input as a probe spec.  */
  cs = *stringp;
  if (cs != NULL && probe_linespec_to_static_ops (&cs) != NULL)
    {
      location = new_probe_location (*stringp);
      *stringp += strlen (*stringp);
    }
  else
    {
      /* Try an address location.  */
      if (*stringp != NULL && **stringp == '*')
	{
	  const char *arg, *orig;
	  CORE_ADDR addr;

	  orig = arg = *stringp;
	  addr = linespec_expression_to_pc (&arg);
	  location = new_address_location (addr, orig, arg - orig);
	  *stringp += arg - orig;
	}
      else
	{
	  /* Everything else is a linespec.  */
	  location = new_linespec_location (stringp, match_type);
	}
    }

  return location;
}

/* See description in location.h.  */

event_location_up
string_to_event_location (const char **stringp,
			  const struct language_defn *language,
			  symbol_name_match_type match_type)
{
  const char *arg, *orig;

  /* Try an explicit location.  */
  orig = arg = *stringp;
  event_location_up location = string_to_explicit_location (&arg, language, NULL);
  if (location != NULL)
    {
      /* It was a valid explicit location.  Advance STRINGP to
	 the end of input.  */
      *stringp += arg - orig;

      /* If the user really specified a location, then we're done.  */
      if (!event_location_empty_p (location.get ()))
	return location;

      /* Otherwise, the user _only_ specified optional flags like
	 "-qualified", otherwise string_to_explicit_location would
	 have thrown an error.  Save the flags for "basic" linespec
	 parsing below and discard the explicit location.  */
      event_location_explicit *xloc
	= dynamic_cast<event_location_explicit *> (location.get ());
      gdb_assert (xloc != nullptr);
      match_type = xloc->explicit_loc.func_name_match_type;
    }

  /* Everything else is a "basic" linespec, address, or probe
     location.  */
  return string_to_event_location_basic (stringp, language, match_type);
}

/* See description in location.h.  */

int
event_location_empty_p (const struct event_location *location)
{
  return location->empty_p ();
}

/* See description in location.h.  */

void
set_event_location_string (struct event_location *location,
			   std::string &&string)
{
  location->as_string = std::move (string);
}

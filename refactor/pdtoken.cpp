/* 
 * (C) Copyright 2001 Diomidis Spinellis.
 *
 * For documentation read the corresponding .h file
 *
 * $Id: pdtoken.cpp,v 1.34 2001/09/02 14:20:54 dds Exp $
 */

#include <iostream>
#include <map>
#include <string>
#include <deque>
#include <stack>
#include <iterator>
#include <fstream>
#include <list>
#include <set>
#include <algorithm>
#include <functional>
#include <cassert>
#include <cstdlib>		// strtol

#include "cpp.h"
#include "fileid.h"
#include "tokid.h"
#include "token.h"
#include "ytab.h"
#include "ptoken.h"
#include "fchar.h"
#include "error.h"
#include "pltoken.h"
#include "pdtoken.h"
#include "tchar.h"
#include "ctoken.h"

bool Pdtoken::at_bol = true;
listPtoken Pdtoken::expand;
mapMacro Pdtoken::macros;		// Defined macros
stackbool Pdtoken::iftaken;		// Taken #ifs
int Pdtoken::skiplevel = 0;		// Level of enclosing #ifs when skipping

static void macro_replace_all(listPtoken& tokens, listPtoken::iterator end, setstring& tabu, bool get_more);

void
Pdtoken::getnext()
{
	Pltoken t;
	setstring tabu;				// For macro replacement

expand_get:
	if (!expand.empty()) {
		Pdtoken n(expand.front());
		*this = n;
		expand.pop_front();
		return;
	}
again:
	t.template getnext<Fchar>();
	if (at_bol) {
		switch (t.get_code()) {
		case SPACE:
		case '\n':
			goto again;
		case '#':
			process_directive();
			goto again;
		default:
			at_bol = false;
		}
	}
	if (skiplevel) {
		if (t.get_code() == '\n')
			at_bol = true;
		else if (t.get_code() == EOF) {
			Error::error(E_ERR, "EOF while processing #if directive");
			*this = t;
			return;
		}
		goto again;
	}
	switch (t.get_code()) {
	case '\n':
		at_bol = true;
		//goto again; To remove newlines at this step
		*this = t;
		break;
	case IDENTIFIER:
		expand.push_front(t);
		tabu.clear();
		macro_replace(expand, expand.begin(), tabu, true);
		goto expand_get;
		// FALLTRHOUGH
	default:
		*this = t;
		break;
	case EOF:
		if (!iftaken.empty())
			Error::error(E_ERR, "EOF while processing #if directive");
		*this = t;
		break;
	}
}


// Consume input up to (and including) the first \n
void
Pdtoken::eat_to_eol()
{
	Pltoken t;

	do {
		t.template getnext<Fchar>();
	} while (t.get_code() != EOF && t.get_code() != '\n');
}


/*
 * Lexical analyser for #if expressions
 */
static listPtoken::iterator eval_ptr;
static listPtoken eval_tokens;
long eval_result;

int
eval_lex()
{
	extern long eval_lval;
	const char *num;
	char *endptr;
	Ptoken t;
	int code;
again:
	if (eval_ptr == eval_tokens.end())
		return 0;
	switch ((t = (*eval_ptr++)).get_code()) {
	case '\n':
	case SPACE:
		goto again;
	case PP_NUMBER:
		num = t.get_val().c_str();
		eval_lval = strtol(num, &endptr, 0);
		if (*endptr == 0)
			return (INT_CONST);
		else
			return (FLOAT_CONST);	// Should be flagged as error
	case CHAR_LITERAL:
		{
		const string& s = t.get_val();
		string::const_iterator si;

		//cout << "char:[" << s << "]\n";
		si = s.begin();
		eval_lval = unescape_char(s, si);
		if (si != s.end())
			Error::error(E_ERR, "Illegal characters in character escape sequence");
		return (INT_CONST);
		}
	default:
		return (t.get_code());
	}
}

#ifdef ndef
// Used for debugging the eval_lex function after renaming it to eval_lex_real
int
eval_lex()
{
	int l = eval_lex_real();
	cout << "Eval lex returns " << l << "\n";
	return (l);
}
#endif

/*
 * Read tokens comprising a cpp expression up to the newsline and return
 * its value.
 * Algorithm:
 * -. Read tokens.
 * -. Process defined operator
 * -. Macro-expand sequence
 * -. Replace all identifiers with 0
 * -. Parse and evaluate sequence
 */
int
eval()
{
	extern eval_parse();
	Pltoken t;

	// Read eval_tokens
	eval_tokens.clear();
	do {
		t.template getnext<Fchar>();
		eval_tokens.push_back(t);
	} while (t.get_code() != EOF && t.get_code() != '\n');

	//cout << "Tokens after reading:\n";
	//copy(eval_tokens.begin(), eval_tokens.end(), ostream_iterator<Ptoken>(cout));

	// Process the "defined" operator
	listPtoken::iterator i, arg, last, i2;
	for (i = eval_tokens.begin(); 
	     (i = i2 = find_if(i, eval_tokens.end(), compose1(bind2nd(equal_to<string>(),"defined"), mem_fun_ref(&Ptoken::get_val)))) != eval_tokens.end(); ) {
	     	bool need_bracket = false;
		i2++;
		arg = i2 = find_if(i2, eval_tokens.end(), not1(mem_fun_ref(&Ptoken::is_space)));
		if (arg != eval_tokens.end() && (*arg).get_code() == '(') {
			i2++;
			arg = i2 = find_if(i2, eval_tokens.end(), not1(mem_fun_ref(&Ptoken::is_space)));
			need_bracket = true;
		}
		if (arg == eval_tokens.end() || (*arg).get_code() != IDENTIFIER) {
			Error::error(E_ERR, "No identifier following defined operator");
			return 1;
		}
		if (need_bracket) {
			i2++;
			last = find_if(i2, eval_tokens.end(), not1(mem_fun_ref(&Ptoken::is_space)));
			if (last == eval_tokens.end() || (*last).get_code() != ')') {
				Error::error(E_ERR, "Missing close bracket in defined operator");
				return 1;
			}
		} else
			last = arg;
		last++;
		// We are about to erase it
		string val = (*arg).get_val();
		// cout << "val:" << val << "\n";
		mapMacro::const_iterator mi = Pdtoken::macros.find(val);
		if (mi != Pdtoken::macros.end())
			unify(*arg, (*mi).second.name_token);
		eval_tokens.erase(i, last);
		eval_tokens.insert(last, Ptoken(PP_NUMBER, mi == Pdtoken::macros.end() ? "0" : "1"));
		i = last;
	}
	//cout << "Tokens after defined:\n";
	//copy(eval_tokens.begin(), eval_tokens.end(), ostream_iterator<Ptoken>(cout));

	// Macro replace
	setstring tabu;
	macro_replace_all(eval_tokens, eval_tokens.end(), tabu, false);

	// Change remaining identifiers to 0
	for (i = eval_tokens.begin(); 
	     (i = find_if(i, eval_tokens.end(), compose1(bind2nd(equal_to<int>(),IDENTIFIER), mem_fun_ref(&Ptoken::get_code)))) != eval_tokens.end(); )
	     	*i = Ptoken(PP_NUMBER, "0");
	eval_ptr = eval_tokens.begin();
	// cout << "Tokens before parsing:\n";
	// copy(eval_tokens.begin(), eval_tokens.end(), ostream_iterator<Ptoken>(cout));
	if (eval_parse() != 0) {
		Error::error(E_ERR, "Syntax error in preprocessor expression");
		return 1;
	}
	// cout << "Eval returns: " << eval_result << "\n";
	return (eval_result);
}

/*
 * Algorithm for processing #if* #else #elif #endif sequences:
 * Each #if is evaluated and the result is pushed on the iftaken stack.
 * If false skip to next matching control.
 * At #elif check iftaken.top(): if true, skip to next matching control,
 * if false, pop stack and work like an if.
 * At #else check iftaken.top(): if true, skip to next matching control,
 * if false continue processing input.
 * At #endif execute iftaken.pop()
 *
 * Skipping is performed by setting skiplevel=1 and looking for a 
 * #e* when skiplevel == 1.
 * While skiplevel > 0 each #if* results in skiplevel++, each #endif in 
 * skiplevel--.
 * Skiplevel handling:
 * 0 normal processing
 * >= 1 skipping where value is the number of enclosing #if blocks
 */
void
Pdtoken::process_if()
{
	if (skiplevel)
		skiplevel++;
	else {
		bool eval_res = eval();
		iftaken.push(eval_res);
		skiplevel = eval_res ? 0 : 1;
	}
}

void
Pdtoken::process_elif()
{
	if (iftaken.empty()) {
		Error::error(E_ERR, "Unbalanced #elif");
		eat_to_eol();
		return;
	}
	if (skiplevel > 1)
		return;
	if (iftaken.top())
		skiplevel = 1;
	else {
		iftaken.pop();
		skiplevel = 0;
		process_if();
	}
}

void
Pdtoken::process_else()
{
	if (iftaken.empty()) {
		Error::error(E_ERR, "Unbalanced #else");
		eat_to_eol();
		return;
	}
	if (skiplevel > 1)
		return;
	if (iftaken.top()) {
		skiplevel = 1;
		return;
	}
	skiplevel = 0;
	eat_to_eol();
}

void
Pdtoken::process_endif()
{
	if (iftaken.empty()) {
		Error::error(E_ERR, "Unbalanced #endif");
		eat_to_eol();
		return;
	}
	if (skiplevel <= 1)
		iftaken.pop();
	if (skiplevel >= 1)
		skiplevel--;
	eat_to_eol();
}

void
Pdtoken::process_include()
{
	Pltoken::set_context(cpp_include);
	eat_to_eol();
}

void
Pdtoken::process_define()
{
	Macro m;
	string name;
	typedef map <string, Token> mapToken;	// To unify args with body
	mapToken args;
	Pltoken t;

	Pltoken::set_context(cpp_define);
	t.template getnext_nospc<Fchar>();
	if (t.get_code() != IDENTIFIER) {
		Error::error(E_ERR, "Invalid macro name");
		eat_to_eol();
		return;
	}
	m.name_token = t;
	name = t.get_val();
	t.template getnext<Fchar>();	// Space is significant: a(x) vs a (x)
	m.is_function = false;
	if (t.get_code() == '(') {
		// Function-like macro
		m.is_function = true;
		t.template getnext_nospc<Fchar>();
		if (t.get_code() != ')')
			// Formal args follow; gather them
			for (;;) {
				if (t.get_code() != IDENTIFIER) {
					Error::error(E_ERR, "Invalid macro parameter name");
					eat_to_eol();
					return;
				}
				args[t.get_val()] = t;
				m.formal_args.push_back(t);
				t.template getnext_nospc<Fchar>();
				if (t.get_code() == ')')
					break;
				if (t.get_code() != ',') {
					Error::error(E_ERR, "Invalid macro parameter punctuation");
					eat_to_eol();
					return;
				}
				t.template getnext_nospc<Fchar>();
			}
	}
	// Continue gathering macro body
	for (int i = 0;; i++) {
		// Non-leading whitespace is significant for comparing same 
		// definitions.
		if (i == 0)
			t.template getnext_nospc<Fchar>();
		else
			t.template getnext<Fchar>();
		if (t.get_code() == '\n')
			break;
		m.value.push_back(t);
		mapToken::const_iterator i;
		if ((i = args.find(t.get_val())) != args.end())
			unify(t, (*i).second);
	}
	// Remove trailing whitespace
	// Took me three hours to arrive at
	m.value.erase((find_if(m.value.rbegin(), m.value.rend(), not1(mem_fun_ref(&Ptoken::is_space)))).base(), m.value.end());
	// cout << "Macro definition :\n";
	// copy(m.value.begin(), m.value.end(), ostream_iterator<Ptoken>(cout));
	// Check that the new macro is not different from an older definition
	mapMacro::const_iterator i = macros.find(name);
	if (i != macros.end() && (*i).second != m)
		Error::error(E_WARN, "Duplicate (different) macro definition");
	macros[name] = m;
}

void
Pdtoken::process_undef()
{
	Pltoken t;

	t.template getnext_nospc<Fchar>();
	if (t.get_code() != IDENTIFIER) {
		Error::error(E_ERR, "Invalid macro name");
		eat_to_eol();
		return;
	}
	mapMacro::iterator mi;
	if ((mi = Pdtoken::macros.find(t.get_val())) != Pdtoken::macros.end()) {
		unify(t, (*mi).second.name_token);
		Pdtoken::macros.erase(mi);
	}
	eat_to_eol();
}

void
Pdtoken::process_line()
{
	eat_to_eol();
}

void
Pdtoken::process_error()
{
	eat_to_eol();
}

void
Pdtoken::process_pragma()
{
	eat_to_eol();
}

void
Pdtoken::process_directive()
{
	Pltoken t;
	bool if_val;

	t.template getnext_nospc<Fchar>();
	if (t.get_code() == '\n')		// Empty directive
		return;
	if (t.get_code() != IDENTIFIER) {
		Error::error(E_ERR, "Preprocessor syntax");
		eat_to_eol();
		return;
	}
	if (t.get_val() == "define")
		process_define();
	else if (t.get_val() == "include")
		process_include();
	else if (t.get_val() == "if")
		process_if();
	else if (t.get_val() == "ifdef")
		process_if();
	else if (t.get_val() == "ifndef")
		process_if();
	else if (t.get_val() == "elif")
		process_elif();
	else if (t.get_val() == "else")
		process_else();
	else if (t.get_val() == "endif")
		process_endif();
	else if (t.get_val() == "undef")
		process_undef();
	else if (t.get_val() == "line")
		process_line();
	else if (t.get_val() == "error")
		process_error();
	else if (t.get_val() == "pragma")
		process_pragma();
	else
		Error::error(E_ERR, "Unknown preprocessor directive: " + t.get_val());
}


/*
 * Return a macro argument token from tokens position pos.
 * Used by gather_args.
 * If get_more is true when tokens is exhausted read using pltoken::getnext
 * Update pos to the first token not gathered.
 * If want_space is true return spaces, otherwise discard them
 */
Ptoken
arg_token(listPtoken& tokens, listPtoken::iterator& pos, bool get_more, bool want_space)
{
	if (want_space) {
		if (pos != tokens.end())
			return (*pos++);
		if (get_more) {
			Pltoken t;
			t.template getnext<Fchar>();
			return (t);
		}
		return Ptoken(EOF, "");
	} else {
		while (pos != tokens.end() && (*pos).is_space())
			pos++;
		if (pos != tokens.end())
			return (*pos++);
		if (get_more) {
			Pltoken t;
			do {
				t.template getnext_nospc<Fchar>();
			} while (t.get_code() != EOF && t.is_space());
			return (t);
		}
		return Ptoken(EOF, "");
	}
}
				
/*
 * Get the macro arguments specified in formal_args, initiallly from pos,
 * then, if get_more is true, from pltoken<fchar>.getnext.
 * Build the map from formal name to argument value args.
 * Update pos to the first token not gathered.
 * Return true if ok, false on error.
 */
static bool
gather_args(const string& name, listPtoken& tokens, listPtoken::iterator& pos, const dequePtoken& formal_args, mapArgval& args, bool get_more)
{
	Ptoken t;
	t = arg_token(tokens, pos, get_more, false);
	assert (t.get_code() == '(');
	dequePtoken::const_iterator i;
	for (i = formal_args.begin(); i != formal_args.end(); i++) {
		listPtoken& v = args[(*i).get_val()];
		char term = (i + 1 == formal_args.end()) ? ')' : ',';
		int bracket = 0;
		// Get a single argument
		for (;;) {
			t = arg_token(tokens, pos, get_more, true);
			if (bracket == 0 && t.get_code() == term)
				break;
			switch (t.get_code()) {
			case '(':
				bracket++;
				break;
			case ')':
				bracket--;
				break;
			case EOF:
				Error::error(E_ERR, "macro [" + name + "]: EOF while reading function macro arguments");
				return (false);
			}
			v.push_back(t);
		}
		// cout << "Gather args returns: " << v << "\n";
	}
	if (formal_args.size() == 0) {
		t = arg_token(tokens, pos, get_more, false);
		if (t.get_code() != ')') {
				Error::error(E_ERR, "macro [" + name + "]: close bracket expected for function-like macro");
			return false;
		}
	}
	return (true);
}


// Return s with all \ and " characters \ escaped
static string
escape(const string& s)
{
	string r;

	for (string::const_iterator i = s.begin(); i != s.end(); i++)
		switch (*i) {
		case '\\':
		case '"':
			r += '\\';
			// FALTHROUGH
		default:
			r += *i;
		}
	return (r);
}

/*
 * Convert a list of tokens into a string as specified by the # operator
 * Multiple spaces are converted to a single space, \ and " are
 * escaped
 */
static Ptoken
stringize(const listPtoken& ts)
{
	string res;
	listPtoken::const_iterator pi;
	bool seen_space = true;		// To delete leading spaces

	for (pi = ts.begin(); pi != ts.end(); pi++) {
		switch ((*pi).get_code()) {
		case '\n':
		case SPACE:
			if (seen_space)
				continue;
			else
				seen_space = true;
			res += ' ';
			break;
		case STRING_LITERAL:
			seen_space = false;
			res += "\\\"" + escape((*pi).get_val()) + "\\\"";
			break;
		case CHAR_LITERAL:
			seen_space = false;
			res += '\'' + escape((*pi).get_val()) + '\'';
			break;
		default:
			seen_space = false;
			res += (*pi).get_val();
			break;
		}
	}
	// Remove trailing spaces
	res.erase((find_if(res.rbegin(), res.rend(), not1(ptr_fun(isspace)))).base(), res.end());
	return (Ptoken(STRING_LITERAL, res));
}


/*
 * Return true if if macro-replacement of *p occuring within v is allowed.
 * According to ANSI 3.8.3.1 p. 91
 * macro replacement is not performed when the argument is preceded by # or ## 
 * or followed by ##.
 * These rules do not take into account space tokens.
 */
bool
macro_replacement_allowed(const dequePtoken& v, dequePtoken::const_iterator p)
{
	dequePtoken::const_iterator i;

	// Check previous first non-white token
	for (i = p; i != v.begin(); ) {
		i--;
		if ((*i).get_code() == '#' || (*i).get_code() == CPP_CONCAT)
			return (false);
		if (!(*i).is_space())
			break;
	}

	// Check next first non-white token
	for (i = p + 1; i != v.end() && (*i).is_space(); i++)
		if ((*i).get_code() == CPP_CONCAT)
			return (false);
	return (true);
}

/*
 * Macro replace all tokens in the sequence from tokens.begin() up to the 
 * "end" iterator
 */
static void
macro_replace_all(listPtoken& tokens, listPtoken::iterator end, setstring& tabu, bool get_more)
{
	listPtoken::iterator ti;
	setstring rescan_tabu(tabu);

	// cout << "Enter replace_all\n";
	for (ti = tokens.begin(); ti != end; ) {
		if ((*ti).get_code() == IDENTIFIER)
			ti = macro_replace(tokens, ti, tabu, get_more);
		else
			ti++;
	}
	// cout << "Exit replace_all\n";
}

/*
 * Check for macro at token position pos and possibly expand it  
 * If a macro is expanded, pos is invalidated and replaced with the replacement 
 * macro value.
 * Macros that are members of the tabu set are not expanded to avoid
 * infinite recursion.
 * If get_more is true, more data can be retrieved from Pltoken::get_next
 * Return the first position in tokens sequence that was not 
 * examined or replaced.
 */
listPtoken::iterator
macro_replace(listPtoken& tokens, listPtoken::iterator pos, setstring tabu, bool get_more)
{
	mapMacro::const_iterator mi;
	const string name = (*pos).get_val();
	#ifdef ndef
	cout << "macro_replace: [" << name << "] tabu: ";
	for (setstring::const_iterator si = tabu.begin(); si != tabu.end(); si++)
		cout << *si << " ";
	cout << "\n";
	#endif
	if ((mi = Pdtoken::macros.find(name)) == Pdtoken::macros.end() || !(*pos).can_replace())
		return (++pos);
	if (tabu.find(name) != tabu.end()) {
		(*pos).set_nonreplaced();
		return (++pos);
	}
	const Macro& m = mi->second;
	if (m.is_function) {
		// Peek for a left bracket, if not found this is not a macro
		listPtoken::iterator peek = pos;
		peek++;
		while (peek != tokens.end() && (*peek).is_space())
			peek++;
		if (peek == tokens.end()) {
			if (get_more) {
				Pltoken t;
				do {
					t.template getnext<Fchar>();
					tokens.push_back(t);
				} while (t.get_code() != EOF && t.is_space());
				if (t.get_code() != '(')
					return (++pos);
			} else
				return (++pos);
		} else if ((*peek).get_code() != '(')
			return (++pos);
	}
	// cout << "replacing for " << name << "\n";
	unify(*pos, (*mi).second.name_token);
	listPtoken::iterator expand_start = pos;
	expand_start++;
	tokens.erase(pos);
	pos = expand_start;
	if (m.is_function) {
		mapArgval args;			// Map from formal name to value
		bool do_stringize;

		expand_start = pos;
		if (!gather_args(name, tokens, pos, m.formal_args, args, get_more))
			return (pos);
		tokens.erase(expand_start, pos);
		dequePtoken::const_iterator i;
		// Substitute with macro's replacement value
		for(i = m.value.begin(); i != m.value.end(); i++) {
			// Is it a stringizing operator ?
			if ((*i).get_code() == '#') {
				if (i + 1 == m.value.end()) {
					Error::error(E_ERR,  "Application of macro \"" + name + "\": operator # at end of macro pattern");
					return (pos);
				}
				do_stringize = true;
				// Advance to next non-space
				do {
					i++;
				} while ((*i).is_space());
			} else
				do_stringize = false;
			mapArgval::const_iterator ai;
			// Is it a formal argument?
			if ((ai = args.find((*i).get_val())) != args.end()) {
				if (macro_replacement_allowed(m.value, i)) {
					// Allowed, macro replace the parameter
					// in temporary var arg, and
					// copy that back to the main
					listPtoken arg((*ai).second.begin(), (*ai).second.end());
					// cout << "Arg macro:" << arg << "---\n";
					// See comment in macro_replace_all
					macro_replace_all(arg, arg.end(), tabu, false);
					// cout << "Arg macro result:" << arg << "---\n";
					copy(arg.begin(), arg.end(), inserter(tokens, pos));
				} else if (do_stringize)
					tokens.insert(pos, stringize((*ai).second));
				else
					copy((*ai).second.begin(), (*ai).second.end(), inserter(tokens, pos));
			} else {
				// Not a formal argument, plain replacement
				// Check for misplaced # operator (3.8.3.2)
				if (do_stringize)
					Error::error(E_WARN, "Application of macro \"" + name + "\": operator # only valid before macro parameters");
				tokens.insert(pos, *i);
			}
		}
	} else {
		// Object-like macro
		tokens.insert(pos, m.value.begin(), m.value.end());
	}

	// Check and apply CPP_CONCAT (ANSI 3.8.3.3)
	listPtoken::iterator ti, next;
	for (ti = tokens.begin(); ti != tokens.end() && ti != pos; ti = next) {
		if ((*ti).get_code() == CPP_CONCAT && ti != tokens.begin()) {
			listPtoken::iterator left = tokens.end();
			listPtoken::iterator right = tokens.end();
			listPtoken::iterator i;

			// Find left non-space operand
			for (i = ti; i != tokens.begin(); ) {
				i--;
				if (!(*i).is_space()) {
					left = i;
					break;
				}
			}
			// Find right non-space operand
			for (i = ti;; ) {
				i++;
				if (i == tokens.end() || i == pos)
					break;
				if (!(*i).is_space()) {
					right = i;
					break;
				}
			}
			if (left != tokens.end() && right != tokens.end() && right != pos) {
				next = right;
				next++;
				Tchar::clear();
				Tchar::push_input(*left);
				Tchar::push_input(*(right));
				// cout << "concat A:" << *left << "B: " << *right << "\n";
				Tchar::rewind_input();
				tokens.erase(left, next);
				for (;;) {
					Pltoken t;
					t.template getnext<Tchar>();
					if (t.get_code() == EOF)
						break;
					// cout << "Result: " << t ;
					tokens.insert(next, t);
				}
			} else {
				Error::error(E_ERR, "Missing operands for ## operator");
			}
		} else {
			next = ti;
			next++;
		}
	}
	tabu.insert(name);
	// cout << "Rescan-" << name << "\n";
	macro_replace_all(tokens, pos, tabu, get_more);
	// cout << "Rescan ends\n";
	return (pos);
}

static inline bool
space_eq(Ptoken& a, Ptoken& b)
{
	return a.is_space() && b.is_space();
}

// True if two macro definitions are the same
inline bool
operator ==(const Macro& a, const Macro& b)
{
	if (a.is_function != b.is_function || a.formal_args != b.formal_args)
		return false;
	
	// Remove consecutive spaces
	dequePtoken va(a.value);
	dequePtoken vb(b.value);
	va.erase(unique(va.begin(), va.end(), space_eq), va.end());
	vb.erase(unique(vb.begin(), vb.end(), space_eq), vb.end());
	return (va == vb);
}


#ifdef UNIT_TEST

main()
{
	Fchar::set_input("test/pdtest.c");

	for (;;) {
		Pdtoken t;

		t.getnext();
		if (t.get_code() == EOF)
			break;
		cout << t;
	}
	cout << "Tokid map:\n";
	cout << tokid_map;

	return (0);
}
#endif /* UNIT_TEST */

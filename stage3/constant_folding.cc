/*
 *  matiec - a compiler for the programming languages defined in IEC 61131-3
 *
 *  Copyright (C) 2003-2011  Mario de Sousa (msousa@fe.up.pt)
 *  Copyright (C) 2012       Manuele Conti (conti.ma@alice.it)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * This code is made available on the understanding that it will not be
 * used in safety-critical situations without a full and competent review.
 */

/*
 * An IEC 61131-3 compiler.
 *
 * Based on the
 * FINAL DRAFT - IEC 61131-3, 2nd Ed. (2001-12-10)
 *
 */



/* TODO: 
 *         - Add support for comparison (= and !=) of enumeration literals!
 *              We will need to add another const_value entry to the symbol_c, containing the 
 *              possible enumeration value of the enum constant!
 *              Doing this will allow us to more easily implement a constant_propagation_c later on!
 *
 *         - Add support for comparison (= and !=) of the exact same variable
 *                (e.g. if (int_v = int_v) then ...)
 */



/* Do constant folding...
 *
 * I.e., Determine the value of all expressions in which only constant values (i.e. literals) are used.
 * The (constant) result of each operation is stored (annotated) in the respective operation symbol 
 * (e.g.: add_expression_c) in the abstract syntax tree,
 *
 * For example:
 *       2 + 3         -> the constant value '5'    is stored in the add_expression_c symbol.
 *       22.2 - 5.0    -> the constant value '17.2' is stored in the add_expression_c symbol.
 *       etc...
 *
 *
 * NOTE 1 
 *      Some operations and constants can have multiple data types. For example,
 *        1 AND 0
 *      may be either a BOOL, BYTE, WORD or LWORD.
 *
 *      The same happens with 
 *        1 + 2
 *      which may be signed (e.g. INT) or unsigned (UINT)
 *
 *      For the above reason, instead of storing a single constant value, we actually store 4:
 *        - bool
 *        - uint64
 *        -  int64
 *        - real64
 *
 *      Additionally, since the result of an operation may result in an overflow, we actually
 *      store the result inside a struct (defined in absyntax.hh)
 *
 *             ** During stage 3 (semantic analysis/checking) we will be doing constant folding.
 *              * That algorithm will anotate the abstract syntax tree with the result of operations
 *              * on literals (i.e. 44 + 55 will store the result 99).
 *              * Since the same source code (e.g. 1 + 0) may actually be a BOOL or an ANY_INT,
 *              * or an ANY_BIT, we need to handle all possibilities, and determine the result of the
 *              * operation assuming each type.
 *              * For this reason, we have one entry for each possible type, with some expressions
 *              * having more than one entry filled in!
 *              **
 *             typedef enum { cs_undefined,   // not defined --> const_value is not valid!
 *                            cs_const_value, // const value is valid
 *                            cs_overflow     // result produced overflow or underflow --> const_value is not valid!
 *                          } const_status_t;
 *    
 *             typedef struct {
 *                 const_status_t status;
 *                 real64_t       value; 
 *             } const_value_real64_t;
 *             const_value_real64_t *const_value_real64; // when NULL --> UNDEFINED
 *             
 *             typedef struct {
 *                 const_status_t status;
 *                 int64_t        value; 
 *             } const_value_int64_t;
 *             const_value_int64_t *const_value_int64; // when NULL --> UNDEFINED
 *             
 *             typedef struct {
 *                 const_status_t status;
 *                 uint64_t       value; 
 *             } const_value_uint64_t;
 *             const_value_uint64_t *const_value_uint64; // when NULL --> UNDEFINED
 *             
 *             typedef struct {
 *                 const_status_t status;
 *                 bool           value; 
 *             } const_value_bool_t;
 *             const_value_bool_t *const_value_bool; // when NULL --> UNDEFINED
 *
 *
 *
 * NOTE 2 
 *    This file does not print out any error messages!
 *    We cannot really print out error messages when we find an overflow. Since each operation
 *    (symbol in the abstract syntax tree for that operation) will have up to 4 constant results,
 *    it may happen that some of them overflow, while other do not.
 *    We must wait for data type checking to determine the exact data type of each expression
 *    before we can decide whether or not we should print out an overflow error message.
 *
 *    For this reason, this visitor merely annotates the abstract syntax tree, and leaves the
 *    actually printing of errors for the print_datatype_errors_c class!
 *
 * NOTE 3
 *    Constant Folding class is extended with a implementation constant propagation algorithm
 *    by Mario de Sousa.
 *    Main idea is not to implement a general constant propagation algorithm but to reinterpret it
 *    for visitor classes.
 *    We declared a hash map, it contains a variables list linked with current constant values.
 *    During expression evaluation we can retrieve a constant value to symbolic variables getting it from the map.
 *    Also at join source points we use a meet semilattice rules to merge current values between a block
 *    and adjacent block.
 *
 */

#include "constant_folding.hh"
#include <stdlib.h> /* required for malloc() */

#include <string.h>  /* required for strlen() */
// #include <stdlib.h>  /* required for atoi() */
#include <errno.h>   /* required for errno */

#include "../main.hh" // required for uint8_t, real_64_t, ..., and the macros NAN, INFINITY, INT8_MAX, REAL32_MAX, ... */






#define FIRST_(symbol1, symbol2) (((symbol1)->first_order < (symbol2)->first_order)   ? (symbol1) : (symbol2))
#define  LAST_(symbol1, symbol2) (((symbol1)->last_order  > (symbol2)->last_order)    ? (symbol1) : (symbol2))

#define STAGE3_ERROR(error_level, symbol1, symbol2, ...) {                                                                  \
  if (current_display_error_level >= error_level) {                                                                         \
    fprintf(stderr, "%s:%d-%d..%d-%d: error: ",                                                                             \
            FIRST_(symbol1,symbol2)->first_file, FIRST_(symbol1,symbol2)->first_line, FIRST_(symbol1,symbol2)->first_column,\
                                                 LAST_(symbol1,symbol2) ->last_line,  LAST_(symbol1,symbol2) ->last_column);\
    fprintf(stderr, __VA_ARGS__);                                                                                           \
    fprintf(stderr, "\n");                                                                                                  \
    error_count++;                                                                                                     \
  }                                                                                                                         \
}


#define STAGE3_WARNING(symbol1, symbol2, ...) {                                                                             \
    fprintf(stderr, "%s:%d-%d..%d-%d: warning: ",                                                                           \
            FIRST_(symbol1,symbol2)->first_file, FIRST_(symbol1,symbol2)->first_line, FIRST_(symbol1,symbol2)->first_column,\
                                                 LAST_(symbol1,symbol2) ->last_line,  LAST_(symbol1,symbol2) ->last_column);\
    fprintf(stderr, __VA_ARGS__);                                                                                           \
    fprintf(stderr, "\n");                                                                                                  \
    warning_found = true;                                                                                                   \
}












#define SET_CVALUE(dtype, symbol, new_value)  ((symbol)->const_value._##dtype.set(new_value))
#define GET_CVALUE(dtype, symbol)             ((symbol)->const_value._##dtype.get())
#define SET_OVFLOW(dtype, symbol)             ((symbol)->const_value._##dtype.set_overflow())
#define SET_NONCONST(dtype, symbol)           ((symbol)->const_value._##dtype.set_nonconst())

#define VALID_CVALUE(dtype, symbol)           ((symbol)->const_value._##dtype.is_valid())
#define IS_OVFLOW(dtype, symbol)              ((symbol)->const_value._##dtype.is_overflow())
#define IS_NONCONST(dtype, symbol)            ((symbol)->const_value._##dtype.is_nonconst())
#define ISZERO_CVALUE(dtype, symbol)          ((symbol)->const_value._##dtype.is_zero())


#define ISEQUAL_CVALUE(dtype, symbol1, symbol2) \
	(VALID_CVALUE(dtype, symbol1) && VALID_CVALUE(dtype, symbol2) && (GET_CVALUE(dtype, symbol1) == GET_CVALUE(dtype, symbol2))) 

#define DO_BINARY_OPER(oper_type, operation, res_type, operand1, operand2) {                                              \
	if      (VALID_CVALUE(oper_type, operand1) && VALID_CVALUE(oper_type, operand2))                                  \
		{SET_CVALUE(res_type, symbol, GET_CVALUE(oper_type, operand1) operation GET_CVALUE(oper_type, operand2));}\
	else if (IS_OVFLOW   (oper_type, operand1) || IS_OVFLOW   (oper_type, operand2))                                  \
		{SET_OVFLOW(res_type, symbol);}  /* does it really make sense to set OVFLOW when restype is boolean??  */ \
	else if (IS_NONCONST (oper_type, operand1) || IS_NONCONST (oper_type, operand2))                                  \
		{SET_NONCONST(res_type, symbol);}                                                                         \
}

#define DO_UNARY_OPER(dtype, operation, operand) {                                                                        \
	if      (VALID_CVALUE(dtype, operand))                                                                            \
		{SET_CVALUE(dtype, symbol, operation GET_CVALUE(dtype, operand));}                                        \
	else if (IS_OVFLOW   (dtype, operand))                                                                            \
		{SET_OVFLOW(dtype, symbol);}                                                                              \
	else if (IS_NONCONST (dtype, operand))                                                                            \
		{SET_NONCONST(dtype, symbol);}                                                                            \
}

/* Constant Propagation: Rules for Meet from "Cooper K., Torczon L. - Engineering a Compiler, Second Edition - 2011"
 * at 9.3 Static Single-Assignment Form  page 517
 * - any * undefined = any
 * - any * non_const = non_const
 * - constant * constant = constant  (if equal)
 * - constant * constant = non_const (if not equal)
 */
#define COMPUTE_MEET_SEMILATTICE(dtype, c1, c2, resValue) {\
		if (( c1._##dtype.get()  != c2._##dtype.get() && c2._##dtype.is_valid() && c1._##dtype.is_valid()) ||\
		    ( c1._##dtype.is_nonconst() && c2._##dtype.is_valid() ) ||\
		    ( c2._##dtype.is_nonconst() && c1._##dtype.is_valid() )) {\
			resValue._##dtype.set_nonconst();\
		} else {\
			resValue._##dtype.set(c1._##dtype.get());\
		}\
}

typedef std::map <std::string, const_value_c> map_values_t;

static map_values_t values;



/***********************************************************************/
/***********************************************************************/
/***********************************************************************/
/***            convert string to numerical value                    ***/
/***********************************************************************/
/***********************************************************************/
/***********************************************************************/



  /* To allow the compiler to be portable, we cannot assume that int64_t is mapped onto long long int,
   * so we cannot call strtoll() and strtoull() in extract_int64() and extract_uint64().
   *
   * So, we create our own strtouint64() and strtoint64() functions.
   * (We actually call them matiec_strtoint64() so they will not clash with any function
   *  that may be added to the standard library in the future).
   * We actually create several of each, and let the compiler choose which is the correct one,
   * by having it resolve the call to the overloaded function. For the C++ compiler to be able
   * to resolve this ambiguity, we need to add a dummy parameter to each function!
   *
   * TODO: support platforms (where the compiler will run) in which int64_t is mapped onto int !!
   *       Is this really needed?
   *       Currently, when trying to compile matiec on sych a platform, the C++ compiler will not
   *       find any apropriate matiec_strtoint64() to call, so matiec will not be able to be compiled.
   *       If you need this, you are welcome to fix it yourself...
   */
static  int64_t matiec_strtoint64 (         long      int *dummy, const char *nptr, char **endptr, int base) {return strtol  (nptr, endptr, base);}
static  int64_t matiec_strtoint64 (         long long int *dummy, const char *nptr, char **endptr, int base) {return strtoll (nptr, endptr, base);}
  
static uint64_t matiec_strtouint64(unsigned long      int *dummy, const char *nptr, char **endptr, int base) {return strtoul (nptr, endptr, base);}
static uint64_t matiec_strtouint64(unsigned long long int *dummy, const char *nptr, char **endptr, int base) {return strtoull(nptr, endptr, base);}


/* extract the value of an integer from an integer_c object !! */
/* NOTE: it must ignore underscores! */
/* NOTE: To follow the basic structure used throughout the compiler's code, we should really be
 * writing this as a visitor_c (and do away with the dynamic casts!), but since we only have 3 distinct 
 * symbol class types to handle, it is probably easier to read if we write it as a standard function... 
 */
int64_t extract_int64_value(symbol_c *sym, bool *overflow) {
  int64_t      ret;
  std::string  str = "";
  char        *endptr;
  const char  *value = NULL;
  int          base;
  integer_c         *integer;
  hex_integer_c     *hex_integer;
  octal_integer_c   *octal_integer;
  binary_integer_c  *binary_integer;

   if       ((integer        = dynamic_cast<integer_c *>(sym))        != NULL) {value = integer       ->value + 0; base = 10;}
   else  if ((hex_integer    = dynamic_cast<hex_integer_c *>(sym))    != NULL) {value = hex_integer   ->value + 3; base = 16;}
   else  if ((octal_integer  = dynamic_cast<octal_integer_c *>(sym))  != NULL) {value = octal_integer ->value + 2; base =  8;}
   else  if ((binary_integer = dynamic_cast<binary_integer_c *>(sym)) != NULL) {value = binary_integer->value + 2; base =  2;}
   else  ERROR;

  for(unsigned int i = 0; i < strlen(value); i++)
    if (value[i] != '_')  str += value[i];

  errno = 0; // since strtoXX() may legally return 0, we must set errno to 0 to detect errors correctly!
  ret = matiec_strtoint64((int64_t *)NULL, str.c_str(), &endptr, base);
  if (overflow != NULL)
    *overflow = (errno == ERANGE);
  if (((errno != 0) && (errno != ERANGE)) || (*endptr != '\0'))
    ERROR;

  return ret;
}



uint64_t extract_uint64_value(symbol_c *sym, bool *overflow) {
  uint64_t     ret;
  std::string  str = "";
  char        *endptr;
  const char  *value = NULL;
  int          base;
  integer_c         *integer;
  hex_integer_c     *hex_integer;
  octal_integer_c   *octal_integer;
  binary_integer_c  *binary_integer;

   if       ((integer        = dynamic_cast<integer_c *>(sym))        != NULL) {value = integer       ->value + 0; base = 10;}
   else  if ((hex_integer    = dynamic_cast<hex_integer_c *>(sym))    != NULL) {value = hex_integer   ->value + 3; base = 16;}
   else  if ((octal_integer  = dynamic_cast<octal_integer_c *>(sym))  != NULL) {value = octal_integer ->value + 2; base =  8;}
   else  if ((binary_integer = dynamic_cast<binary_integer_c *>(sym)) != NULL) {value = binary_integer->value + 2; base =  2;}
   else  ERROR;

  for(unsigned int i = 0; i < strlen(value); i++)
    if (value[i] != '_')  str += value[i];

  errno = 0; // since strtoXX() may legally return 0, we must set errno to 0 to detect errors correctly!
  ret = matiec_strtouint64((uint64_t *)NULL, str.c_str(), &endptr, base);
  if (overflow != NULL)
    *overflow = (errno == ERANGE);
  if (((errno != 0) && (errno != ERANGE)) || (*endptr != '\0'))
    ERROR;

  return ret;
}



/* extract the value of a real from an real_c object !! */
/* NOTE: it must ignore underscores! */
/* From iec_bison.yy
 *  real:
 *   real_token		{$$ = new real_c($1, locloc(@$));}
 * | fixed_point_token	{$$ = new real_c($1, locloc(@$));}
 *
 * From iec_flex.ll
 * {real}			{yylval.ID=strdup(yytext); return real_token;}
 * {fixed_point}		{yylval.ID=strdup(yytext); return fixed_point_token;}
 *
 * real		{integer}\.{integer}{exponent}
 * fixed_point		{integer}\.{integer}
 * exponent        [Ee]([+-]?){integer}
 * integer         {digit}((_?{digit})*)
 */
real64_t extract_real_value(symbol_c *sym, bool *overflow) {
  std::string str = "";
  real_c *real_sym;
  fixed_point_c *fixed_point_sym;
  char   *endptr;
  real64_t ret;

  if ((real_sym = dynamic_cast<real_c *>(sym)) != NULL) {
	for(unsigned int i = 0; i < strlen(real_sym->value); i++)
      if (real_sym->value[i] != '_') str += real_sym->value[i];
  }
  else if ((fixed_point_sym = dynamic_cast<fixed_point_c *>(sym)) != NULL) {
    for(unsigned int i = 0; i < strlen(fixed_point_sym->value); i++)
      if (fixed_point_sym->value[i] != '_') str += fixed_point_sym->value[i];
  }
  else ERROR;
    
  errno = 0; // since strtoXX() may legally return 0, we must set errno to 0 to detect errors correctly!
  #if    (real64_tX  == float)
    ret = strtof(str.c_str(),  &endptr);
  #elif  (real64_tX  == double)
    ret = strtod(str.c_str(),  &endptr);
  #elif  (real64_tX  == long_double)
    ret = strtold(str.c_str(), &endptr);
  #else 
    #error Could not determine which data type is being used for real64_t (defined in main.hh). Aborting!
  #endif
  if (overflow != NULL)
    *overflow = (errno == ERANGE);
  if (((errno != 0) && (errno != ERANGE)) || (*endptr != '\0'))
    ERROR;

  return ret;
}





/***********************************************************************/
/***********************************************************************/
/***********************************************************************/
/***        Functions to check for overflow situation                ***/
/***********************************************************************/
/***********************************************************************/
/***********************************************************************/


/* NOTE:
 *   Most of the conditions to detect overflows on signed and unsigned integer operations were adapted from
 *   https://www.securecoding.cert.org/confluence/display/seccode/INT32-C.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow?showComments=false
 *   https://www.securecoding.cert.org/confluence/display/seccode/INT30-C.+Ensure+that+unsigned+integer+operations+do+not+wrap
 */

/* NOTE: If at all possible, all overflow tests are done by pre-condition tests, i.e. tests that 
 *       can be run _before_ the operation is executed, and therefore without accessing the result!
 *
 *       The exception is for real/floating point values, that simply test if the result is NaN (not a number).
 */

/* res = a + b */
static void CHECK_OVERFLOW_uint64_SUM(symbol_c *res, symbol_c *a, symbol_c *b) {
	if (!VALID_CVALUE(uint64, res))
		return;
	/* Test by post-condition: If sum is smaller than either operand => overflow! */
	// if (GET_CVALUE(uint64, res) < GET_CVALUE(uint64, a))
	/* Test by pre-condition: If (UINT64_MAX - a) < b => overflow! */
	if ((UINT64_MAX - GET_CVALUE(uint64, a)) < GET_CVALUE(uint64, b))
		SET_OVFLOW(uint64, res);
}


/* res = a - b */
static void CHECK_OVERFLOW_uint64_SUB(symbol_c *res, symbol_c *a, symbol_c *b) {
	if (!VALID_CVALUE(uint64, res))
		return;
	/* Test by post-condition: If diference is larger than a => overflow! */
	// if (GET_CVALUE(uint64, res) > GET_CVALUE(uint64, a))
	/* Test by pre-condition: if b > a => overflow! */
	if (GET_CVALUE(uint64, b) > GET_CVALUE(uint64, a))
		SET_OVFLOW(uint64, res);
}


/* res = a * b */
static void CHECK_OVERFLOW_uint64_MUL(symbol_c *res, symbol_c *a, symbol_c *b) {
	if (!VALID_CVALUE(uint64, res))
		return;
	/* Test by pre-condition: If (UINT64_MAX / a) < b => overflow! */
	if (0 == GET_CVALUE(uint64, a))
		return; // multiplying by 0 will always result in 0, a valid result!	  
	if ((UINT64_MAX / GET_CVALUE(uint64, a)) < GET_CVALUE(uint64, b))
		SET_OVFLOW(uint64, res);
}


/* res = a / b */
static void CHECK_OVERFLOW_uint64_DIV(symbol_c *res, symbol_c *a, symbol_c *b) {
	if (!VALID_CVALUE(uint64, res))
		return;
	if (GET_CVALUE(uint64, b) == 0) /* division by zero! */
		SET_OVFLOW(uint64, res);
}


/* res = a MOD b */
static void CHECK_OVERFLOW_uint64_MOD(symbol_c *res, symbol_c *a, symbol_c *b) {
	if (!VALID_CVALUE(uint64, res))
		return;
	/* no overflow condition exists, including division by zero, which IEC 61131-3 considers legal for MOD operation! */
	if (false) 
		SET_OVFLOW(uint64, res);
}


/* res = - a */
static void CHECK_OVERFLOW_uint64_NEG(symbol_c *res, symbol_c *a) {
	/* The only legal operation is res = -0, everything else is an overflow! */
	if (VALID_CVALUE(uint64, a) && (GET_CVALUE(uint64, a) != 0))
		SET_OVFLOW(uint64, res);
}






/* res = a + b */
static void CHECK_OVERFLOW_int64_SUM(symbol_c *res, symbol_c *a_ptr, symbol_c *b_ptr) {
	if (!VALID_CVALUE(int64, res))
		return;
	int64_t a = GET_CVALUE(int64, a_ptr);
	int64_t b = GET_CVALUE(int64, b_ptr);
	/* The following test is valid no matter what representation is being used (e.g. two's complement, etc...) */
	if (((b > 0) && (a > (INT64_MAX - b)))
	 || ((b < 0) && (a < (INT64_MIN - b))))
		SET_OVFLOW(int64, res);
}


/* res = a - b */
static void CHECK_OVERFLOW_int64_SUB(symbol_c *res, symbol_c *a_ptr, symbol_c *b_ptr) {
	if (!VALID_CVALUE(int64, res))
		return;
	int64_t a = GET_CVALUE(int64, a_ptr);
	int64_t b = GET_CVALUE(int64, b_ptr);
	/* The following test is valid no matter what representation is being used (e.g. two's complement, etc...) */
	if (((b > 0) && (a < (INT64_MIN + b)))
	 || ((b < 0) && (a > (INT64_MAX + b))))
		SET_OVFLOW(int64, res);
}


/* res = a * b */
static void CHECK_OVERFLOW_int64_MUL(symbol_c *res, symbol_c *a_ptr, symbol_c *b_ptr) {
	if (!VALID_CVALUE(int64, res))
		return;
	int64_t a = GET_CVALUE(int64, a_ptr);
	int64_t b = GET_CVALUE(int64, b_ptr);
	if (   ( (a > 0) &&  (b > 0) &&             (a > (INT64_MAX / b))) 
	    || ( (a > 0) && !(b > 0) &&             (b < (INT64_MIN / a))) 
	    || (!(a > 0) &&  (b > 0) &&             (a < (INT64_MIN / b))) 
	    || (!(a > 0) && !(b > 0) && (a != 0) && (b < (INT64_MAX / a))))
		SET_OVFLOW(int64, res);
}


/* res = a / b */
static void CHECK_OVERFLOW_int64_DIV(symbol_c *res, symbol_c *a_ptr, symbol_c *b_ptr) {
	if (!VALID_CVALUE(int64, res))
		return;
	int64_t a = GET_CVALUE(int64, a_ptr);
	int64_t b = GET_CVALUE(int64, b_ptr);
	if ((b == 0) || ((a == INT64_MIN) && (b == -1)))
		SET_OVFLOW(int64, res);
}


/* res = a MOD b */
static void CHECK_OVERFLOW_int64_MOD(symbol_c *res, symbol_c *a_ptr, symbol_c *b_ptr) {
	if (!VALID_CVALUE(int64, res))
		return;
	int64_t a = GET_CVALUE(int64, a_ptr);
	int64_t b = GET_CVALUE(int64, b_ptr);
	/* IEC 61131-3 standard says IN1 MOD IN2 must be equivalent to
	 *  IF (IN2 = 0) THEN OUT:=0 ; ELSE OUT:=IN1 - (IN1/IN2)*IN2 ; END_IF
	 *
	 * Note that, when IN1 = INT64_MIN, and IN2 = -1, an overflow occurs in the division,
	 * so although the MOD operation should be OK, acording to the above definition, we actually have an overflow!!
	 *
	 * On the other hand, division by 0 is OK!!
	 */
	if ((a == INT64_MIN) && (b == -1))
		SET_OVFLOW(int64, res);
}


/* res = - a */
static void CHECK_OVERFLOW_int64_NEG(symbol_c *res, symbol_c *a) {
	if (!VALID_CVALUE(int64, res))
		return;
	if (GET_CVALUE(int64, a) == INT64_MIN)
		SET_OVFLOW(int64, res);
}




static void CHECK_OVERFLOW_real64(symbol_c *res_ptr) {
	if (!VALID_CVALUE(real64, res_ptr))
		return;
	real64_t res = GET_CVALUE(real64, res_ptr);
	/* NaN => underflow, overflow, number is a higher precision format, is a complex number (IEEE standard) */
	/* The IEC 61131-3 clearly states in section '2.5.1.5.2 Numerical functions':
	 * "It is an error if the result of evaluation of one of these [numerical] functions exceeds the range of values
	 *  specified for the data type of the function output, or if division by zero is attempted."
	 * For this reason, any operation that has as a result a positive or negative inifinity, is also an error!
	 */
	if ((isnan(res)) || (res == INFINITY) || (res == -INFINITY))
		SET_OVFLOW(real64, res_ptr);
}




/***********************************************************************/
/***********************************************************************/
/***********************************************************************/
/***        Functions to execute operations on the const values      ***/
/***********************************************************************/
/***********************************************************************/
/***********************************************************************/

/* static void *handle_cmp(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2, OPERATION) */
#define handle_cmp(symbol, oper1, oper2, operation) {               \
	if ((NULL == oper1) || (NULL == oper2)) return NULL;        \
	DO_BINARY_OPER(  bool, operation, bool, oper1, oper2);     \
	DO_BINARY_OPER(uint64, operation, bool, oper1, oper2);     \
	DO_BINARY_OPER( int64, operation, bool, oper1, oper2);     \
	DO_BINARY_OPER(real64, operation, bool, oper1, oper2);     \
	return NULL;                                                \
}


/* NOTE: the MOVE standard function is equivalent to the ':=' in ST syntax */
static void *handle_move(symbol_c *to, symbol_c *from) {
	if (NULL == from) return NULL;
	to->const_value = from->const_value;
	return NULL;
}


/* unary negation (multiply by -1) */
static void *handle_neg(symbol_c *symbol, symbol_c *oper) {
	if (NULL == oper) return NULL;
	/* NOTE: The oper may never be an integer/real literal, '-1' and '-2.2' are stored as an neg_integer_c/neg_real_c instead.
	 *       Because of this, we MUST NOT handle the INT_MIN special situation that is handled in neg_integer_c visitor!
	 *
	 *       VAR v1, v2, v3 : UINT; END_VAR;
	 *       v1 =  9223372036854775808 ; (* |INT64_MIN| == -INT64_MIN *)   <------ LEGAL
	 *       v2 =  -(-v1);                                                 <------ ILLEGAL (since it -v1 is overflow!)
	 *       v2 =  -(-9223372036854775808 );                               <------ MUST also be ILLEGAL 
	 */
	DO_UNARY_OPER(uint64, -, oper);	CHECK_OVERFLOW_uint64_NEG(symbol, oper);  /* handle the uint_v := -0 situation! */
	DO_UNARY_OPER( int64, -, oper);	CHECK_OVERFLOW_int64_NEG (symbol, oper);
	DO_UNARY_OPER(real64, -, oper);	CHECK_OVERFLOW_real64(symbol);
	return NULL;
}


/* unary boolean negation (NOT) */
static void *handle_not(symbol_c *symbol, symbol_c *oper) {
	if (NULL == oper) return NULL;
	DO_UNARY_OPER(  bool, !, oper);
	DO_UNARY_OPER(uint64, ~, oper);
	return NULL;
}


static void *handle_or (symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(  bool, ||, bool  , oper1, oper2);
	DO_BINARY_OPER(uint64, | , uint64, oper1, oper2);
	return NULL;
}


static void *handle_xor(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(  bool, ^, bool  , oper1, oper2);
	DO_BINARY_OPER(uint64, ^, uint64, oper1, oper2);
	return NULL;
}


static void *handle_and(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(  bool, &&, bool, oper1, oper2);
	DO_BINARY_OPER(uint64, & , uint64, oper1, oper2);
	return NULL;
}


static void *handle_add(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(uint64, +, uint64, oper1, oper2);   CHECK_OVERFLOW_uint64_SUM(symbol, oper1, oper2);
	DO_BINARY_OPER( int64, +,  int64, oper1, oper2);   CHECK_OVERFLOW_int64_SUM (symbol, oper1, oper2);
	DO_BINARY_OPER(real64, +, real64, oper1, oper2);   CHECK_OVERFLOW_real64    (symbol);
	return NULL;
}


static void *handle_sub(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(uint64, -, uint64, oper1, oper2);   CHECK_OVERFLOW_uint64_SUB(symbol, oper1, oper2);
	DO_BINARY_OPER( int64, -,  int64, oper1, oper2);   CHECK_OVERFLOW_int64_SUB (symbol, oper1, oper2);
	DO_BINARY_OPER(real64, -, real64, oper1, oper2);   CHECK_OVERFLOW_real64    (symbol);
	return NULL;
}


static void *handle_mul(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	DO_BINARY_OPER(uint64, *, uint64, oper1, oper2);   CHECK_OVERFLOW_uint64_MUL(symbol, oper1, oper2);
	DO_BINARY_OPER( int64, *,  int64, oper1, oper2);   CHECK_OVERFLOW_int64_MUL (symbol, oper1, oper2);
	DO_BINARY_OPER(real64, *, real64, oper1, oper2);   CHECK_OVERFLOW_real64    (symbol);
	return NULL;
}


static void *handle_div(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	if (ISZERO_CVALUE(uint64, oper2))  {SET_OVFLOW(uint64, symbol);} else {DO_BINARY_OPER(uint64, /, uint64, oper1, oper2); CHECK_OVERFLOW_uint64_DIV(symbol, oper1, oper2);};
	if (ISZERO_CVALUE( int64, oper2))  {SET_OVFLOW( int64, symbol);} else {DO_BINARY_OPER( int64, /,  int64, oper1, oper2); CHECK_OVERFLOW_int64_DIV (symbol, oper1, oper2);};
	if (ISZERO_CVALUE(real64, oper2))  {SET_OVFLOW(real64, symbol);} else {DO_BINARY_OPER(real64, /, real64, oper1, oper2); CHECK_OVERFLOW_real64(symbol);};
	return NULL;
}


static void *handle_mod(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	if ((NULL == oper1) || (NULL == oper2)) return NULL;
	/* IEC 61131-3 standard says IN1 MOD IN2 must be equivalent to
	 *  IF (IN2 = 0) THEN OUT:=0 ; ELSE OUT:=IN1 - (IN1/IN2)*IN2 ; END_IF
	 *
	 * Note that, when IN1 = INT64_MIN, and IN2 = -1, an overflow occurs in the division,
	 * so although the MOD operation should be OK, acording to the above definition, we actually have an overflow!!
	 */
	if (ISZERO_CVALUE(uint64, oper2))  {SET_CVALUE(uint64, symbol, 0);} else {DO_BINARY_OPER(uint64, %, uint64, oper1, oper2); CHECK_OVERFLOW_uint64_MOD(symbol, oper1, oper2);};
	if (ISZERO_CVALUE( int64, oper2))  {SET_CVALUE( int64, symbol, 0);} else {DO_BINARY_OPER( int64, %,  int64, oper1, oper2); CHECK_OVERFLOW_int64_MOD (symbol, oper1, oper2);};
	return NULL;
}


static void *handle_pow(symbol_c *symbol, symbol_c *oper1, symbol_c *oper2) {
	/* NOTE: If the const_value in symbol->r_exp is within the limits of both int64 and uint64, then we do both operations.
	 *       That is OK, as the result should be identicial (we do create an unnecessary CVALUE variable, but who cares?).
	 *       If only one is valid, then that is the oper we will do!
	 */
	if (VALID_CVALUE(real64, oper1) && VALID_CVALUE( int64, oper2))
		SET_CVALUE(real64, symbol, pow(GET_CVALUE(real64, oper1), GET_CVALUE( int64, oper2)));
	if (VALID_CVALUE(real64, oper1) && VALID_CVALUE(uint64, oper2))
		SET_CVALUE(real64, symbol, pow(GET_CVALUE(real64, oper1), GET_CVALUE(uint64, oper2)));
	CHECK_OVERFLOW_real64(symbol);
	return NULL;
}

static map_values_t inner_left_join_values(map_values_t m1, map_values_t m2) {
	map_values_t::const_iterator itr;
	map_values_t ret;

	itr = m1.begin();
	for ( ; itr != m1.end(); ++itr) {
		std::string name = itr->first;
		const_value_c value;

		if (m2.count(name) > 0) {
			const_value_c c1 = itr->second;
			const_value_c c2 = m2[name];
			COMPUTE_MEET_SEMILATTICE (real64, c1, c2, value);
			COMPUTE_MEET_SEMILATTICE (uint64, c1, c2, value);
			COMPUTE_MEET_SEMILATTICE ( int64, c1, c2, value);
			COMPUTE_MEET_SEMILATTICE (  bool, c1, c2, value);
		} else
			value = m1[name];
		ret[name] = value;
	}

	return ret;
}

/***********************************************************************/
/***********************************************************************/
/***********************************************************************/
/***        Helper functions for handling IL instruction lists.      ***/
/***********************************************************************/
/***********************************************************************/
/***********************************************************************/


/* If the cvalues of all the prev_il_intructions have the same VALID value, then set the local cvalue to that value, otherwise, set it to NONCONST! */
#define intersect_prev_CVALUE_(dtype, symbol) {                                                                   \
	symbol->const_value._##dtype = symbol->prev_il_instruction[0]->const_value._##dtype;                      \
	for (unsigned int i = 1; i < symbol->prev_il_instruction.size(); i++) {                                   \
		if (!ISEQUAL_CVALUE(dtype, symbol, symbol->prev_il_instruction[i]))                               \
			{SET_NONCONST(dtype, symbol); break;}                                                     \
	}                                                                                                         \
}

static void intersect_prev_cvalues(il_instruction_c *symbol) {
	if (symbol->prev_il_instruction.empty())
		return;
	intersect_prev_CVALUE_(real64, symbol);
	intersect_prev_CVALUE_(uint64, symbol);
	intersect_prev_CVALUE_( int64, symbol);
	intersect_prev_CVALUE_(  bool, symbol);
}



/***********************************************************************/
/***********************************************************************/
/***********************************************************************/
/***        The constant_folding_c                                   ***/
/***********************************************************************/
/***********************************************************************/
/***********************************************************************/

#if 0
// not currently needed, so comment it out!...
// returns true if both symbols have the same value in all the cvalues
bool constant_folding_c::is_equal_cvalue(symbol_c *symbol_1, symbol_c *symbol_2) {
	if (VALID_CVALUE  (real64, symbol_1) != VALID_CVALUE  (real64, symbol_2)) return false;
	if (VALID_CVALUE  (uint64, symbol_1) != VALID_CVALUE  (uint64, symbol_2)) return false;
	if (VALID_CVALUE  ( int64, symbol_1) != VALID_CVALUE  ( int64, symbol_2)) return false;
	if (VALID_CVALUE  (  bool, symbol_1) != VALID_CVALUE  (  bool, symbol_2)) return false;
	if (VALID_CVALUE  (real64, symbol_1) && !ISEQUAL_CVALUE(real64, symbol_1, symbol_2)) return false;
	if (VALID_CVALUE  (uint64, symbol_1) && !ISEQUAL_CVALUE(uint64, symbol_1, symbol_2)) return false;
	if (VALID_CVALUE  ( int64, symbol_1) && !ISEQUAL_CVALUE( int64, symbol_1, symbol_2)) return false;
	if (VALID_CVALUE  (  bool, symbol_1) && !ISEQUAL_CVALUE(  bool, symbol_1, symbol_2)) return false;
	return true;
}
#endif



constant_folding_c::constant_folding_c(symbol_c *symbol) {
    fixed_init_value_ = false;
    function_pou_ = false;
    error_count = 0;
    warning_found = false;
    current_display_error_level = 0;
    il_operand = NULL;
    search_varfb_instance_type = NULL;
    prev_il_instruction = NULL;
    
    /* check whether the platform on which the compiler is being run implements IEC 559 floating point data types. */
    symbol_c null_symbol;
    if (! (std::numeric_limits<real64_t>::is_iec559) )
        STAGE3_WARNING(&null_symbol, &null_symbol, "The platform running the compiler does not implement IEC 60559 floating point numbers. "
                                                   "Any error and/or warning messages related to overflow/underflow of the result of operations on REAL/LREAL literals "
                                                   "(i.e. constant folding) may themselves be erroneous, although are most probably correct."
                                                   "However, more likely is the possible existance of overflow/underflow errors that are not detected.");
}


constant_folding_c::~constant_folding_c(void) {
}


int constant_folding_c::get_error_count() {
	return error_count;
}


/*********************/
/* B 1.2 - Constants */
/*********************/
/******************************/
/* B 1.2.1 - Numeric Literals */
/******************************/
void *constant_folding_c::visit(real_c *symbol) {
	bool overflow;
	SET_CVALUE(real64, symbol, extract_real_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(real64, symbol);
	return NULL;
}


void *constant_folding_c::visit(integer_c *symbol) {
	bool overflow;
	SET_CVALUE( int64, symbol, extract_int64_value (symbol, &overflow));
	if (overflow) SET_OVFLOW(int64, symbol);
	SET_CVALUE(uint64, symbol, extract_uint64_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(uint64, symbol);
	return NULL;
}


void *constant_folding_c::visit(neg_real_c *symbol) {
	symbol->exp->accept(*this);
	DO_UNARY_OPER(real64, -, symbol->exp); CHECK_OVERFLOW_real64(symbol);
	if (IS_OVFLOW(real64, symbol->exp)) SET_OVFLOW(real64, symbol);
	return NULL;
}



/* | '-' integer	{$$ = new neg_integer_c($2, locloc(@$));} */
void *constant_folding_c::visit(neg_integer_c *symbol) {
	symbol->exp->accept(*this);
	/* Note that due to syntax restrictions, the value of symbol->exp will always be positive. 
	 * However, the following code does not depend on that restriction.
	 */
	/* The remainder of the code (for example, data type checking) considers the neg_integer_c as a leaf of the
	 * abstract syntax tree, and therefore simply ignores the values of neg_integer_c->exp.
	 * For this reason only, and in only this situation, we must guarantee that any 'overflow' situation in 
	 * the cvalue of neg_integer_c->exp is also reflected back to this neg_integer_c symbol.
	 * For the rest of the code we do NOT do this, as it would gurantee that a single overflow deep inside
	 * an expression would imply that the expression itself would also be set to 'overflow' condition.
	 * This in turn would then have the compiler produce a whole load of error messages where they are not wanted!
	 */
	DO_UNARY_OPER(uint64, -, symbol->exp); CHECK_OVERFLOW_uint64_NEG(symbol, symbol->exp);  /* handle the uintv := -0 situation */
	if (IS_OVFLOW(uint64, symbol->exp)) SET_OVFLOW(uint64, symbol);
	DO_UNARY_OPER( int64, -, symbol->exp); CHECK_OVERFLOW_int64_NEG (symbol, symbol->exp);
	if (IS_OVFLOW( int64, symbol->exp)) SET_OVFLOW( int64, symbol);
	/* NOTE 1: INT64_MIN = -(INT64_MAX + 1)   ---> assuming two's complement representation!!!
	 * NOTE 2: if the user happens to want INT_MIN, that value will first be parsed as a positive integer, before being negated here.
	 * However, the positive value cannot be stored inside an int64! So, in this case, we will get the value from the uint64 cvalue.
	 *
	 * This same situation is usually considered an overflow (check handle_neg() function). However, here we have a special
	 * situation. If we do not allow this, then the user would never the able to use the following code:
	 *  VAR v : LINT; END_VAR
	 *    v := -9223372036854775809 ; (* - |INT64_MIN| == INT64_MIN *)
	 */
	// if (INT64_MIN == -INT64_MAX - 1) // We do not really need to check that the platform uses two's complement
	if (VALID_CVALUE(uint64, symbol->exp) && (GET_CVALUE(uint64, symbol->exp) == (uint64_t)INT64_MAX+1)) {
		SET_CVALUE(int64, symbol, INT64_MIN);
	}
	return NULL;
}


void *constant_folding_c::visit(binary_integer_c *symbol) {
	bool overflow;
	SET_CVALUE( int64, symbol, extract_int64_value (symbol, &overflow));
	if (overflow) SET_OVFLOW(int64, symbol);
	SET_CVALUE(uint64, symbol, extract_uint64_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(uint64, symbol);
	return NULL;
}


void *constant_folding_c::visit(octal_integer_c *symbol) {
	bool overflow;
	SET_CVALUE( int64, symbol, extract_int64_value (symbol, &overflow));
	if (overflow) SET_OVFLOW(int64, symbol);
	SET_CVALUE(uint64, symbol, extract_uint64_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(uint64, symbol);
	return NULL;
}


void *constant_folding_c::visit(hex_integer_c *symbol) {
	bool overflow;
	SET_CVALUE( int64, symbol, extract_int64_value (symbol, &overflow));
	if (overflow) SET_OVFLOW(int64, symbol);
	SET_CVALUE(uint64, symbol, extract_uint64_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(uint64, symbol);
	return NULL;
}


/*
integer_literal:
  integer_type_name '#' signed_integer	{$$ = new integer_literal_c($1, $3, locloc(@$));}
| integer_type_name '#' binary_integer	{$$ = new integer_literal_c($1, $3, locloc(@$));}
| integer_type_name '#' octal_integer	{$$ = new integer_literal_c($1, $3, locloc(@$));}
| integer_type_name '#' hex_integer	{$$ = new integer_literal_c($1, $3, locloc(@$));}
*/
// SYM_REF2(integer_literal_c, type, value)
void *constant_folding_c::visit(integer_literal_c *symbol) {
	symbol->value->accept(*this);
	DO_UNARY_OPER( int64, /* none */, symbol->value);
	DO_UNARY_OPER(uint64, /* none */, symbol->value);
	return NULL;
}


void *constant_folding_c::visit(real_literal_c *symbol) {
	symbol->value->accept(*this);
	DO_UNARY_OPER(real64, /* none */, symbol->value);
	return NULL;
}


void *constant_folding_c::visit(bit_string_literal_c *symbol) {
	return NULL;
}


void *constant_folding_c::visit(boolean_literal_c *symbol) {
	symbol->value->accept(*this);
	DO_UNARY_OPER(bool, /* none */, symbol->value);
	return NULL;
}


void *constant_folding_c::visit(boolean_true_c *symbol) {
	SET_CVALUE(bool, symbol, true);
	return NULL;
}


void *constant_folding_c::visit(boolean_false_c *symbol) {
	SET_CVALUE(bool, symbol, false);
	return NULL;
}

/************************/
/* B 1.2.3.1 - Duration */
/********* **************/
void *constant_folding_c::visit(fixed_point_c *symbol) {
	bool overflow;
	SET_CVALUE(real64, symbol, extract_real_value(symbol, &overflow));
	if (overflow) SET_OVFLOW(real64, symbol);
	return NULL;
}

/*********************/
/* B 1.4 - Variables */
/*********************/
// #if DO_CONSTANT_PROPAGATION__
void *constant_folding_c::visit(symbolic_variable_c *symbol) {
	std::string varName = get_var_name_c::get_name(symbol->var_name)->value;
	if (values.count(varName) > 0) 
		symbol->const_value = values[varName];
	return NULL;
}
// #endif  // DO_CONSTANT_PROPAGATION__

void *constant_folding_c::visit(symbolic_constant_c *symbol) {
	std::string varName = get_var_name_c::get_name(symbol->var_name)->value;
	if (values.count(varName) > 0) 
		symbol->const_value = values[varName];
	return NULL;
}


/******************************************/
/* B 1.4.3 - Declaration & Initialisation */
/******************************************/

/* Do the constant folding for VAR_EXTERNAL and VAR_GLOBAL pairs.
 *  This function is called from the declaration_check_c, since it has easy access to the extern<->global pairing information
 *  needed for this function to work.
 */
int constant_folding_c::handle_var_extern_global_pair(symbol_c *extern_var_name, symbol_c *extern_var_decl, symbol_c *global_var_name, symbol_c *global_var_decl) {  
  // the minimum infor we must get to make sense
  if (NULL == global_var_decl) ERROR;
  if (NULL == extern_var_name) ERROR;
  
  symbol_c *init_value = type_initial_value_c::get(global_var_decl);  
  if (NULL == init_value)   return 0; // this is probably a FB datatype, for which no initial value exists! Do nothing and return.
  
  // Do constant folding of the initial value!
  //   This is required since this function may be called before we do the iterative constant folding of the complete library!
  init_value->accept(*this);  
  
  if (NULL != extern_var_name) extern_var_name->const_value = init_value->const_value;
  if (NULL != extern_var_decl) extern_var_decl->const_value = init_value->const_value;  // Note that each external variable declaration has its own datatype specification, so we can set this symbol's const_value too!
  // we could leave the constant folding of the global variable itself for later, when we iteratively visit the whole library, but there is nor harm in doing it now!
  if (NULL != global_var_name) global_var_name->const_value = init_value->const_value;
  if (NULL != global_var_decl) global_var_decl->const_value = init_value->const_value;  // Note that each external variable declaration has its own datatype specification, so we can set this symbol's const_value too!
  return 0;
}


  
void *constant_folding_c::handle_var_decl(symbol_c *var_list, bool fixed_init_value) {
  fixed_init_value_ = fixed_init_value;
  var_list->accept(*this); 
  fixed_init_value_ = false; 
  return NULL;
}

void *constant_folding_c::handle_var_list_decl(symbol_c *var_list, symbol_c *type_decl) {
  type_decl->accept(*this);  // Do constant folding of the initial value, and literals in subranges! (we will probably be doing this multiple times for the same init value, but this is safe as the cvalue is idem-potent)
  symbol_c *init_value = type_initial_value_c::get(type_decl);  
  if (NULL == init_value)   {debug_c::print(type_decl); return NULL;} // this is probably a FB datatype, for which no initial value exists! Do nothing and return.
  init_value->accept(*this); // necessary when handling default initial values, that were not constant folded in the call type_decl->accept(*this)
  
  list_c *list = dynamic_cast<list_c *>(var_list);
  if (NULL == list) ERROR;
  for (int i = 0; i < list->n; i++) {
    token_c *var_name = dynamic_cast<token_c *>(list->elements[i]);
    if (NULL == var_name) {
      if (NULL != dynamic_cast<extensible_input_parameter_c *>(list->elements[i]))
        continue; // this is an extensible standard function. Ignore this variable, and continue!
      // debug_c::print(list->elements[i]);
      ERROR;
    }
    list->elements[i]->const_value = init_value->const_value;
    if (fixed_init_value_) {
      std::string varName = var_name->value;
      values[varName] = init_value->const_value;
    }
  }
  return NULL;
}

//SYM_REF0(constant_option_c)     // Not needed!
//SYM_REF0(retain_option_c)       // Not needed!
//SYM_REF0(non_retain_option_c)   // Not needed!
bool constant_folding_c::is_constant(symbol_c *option) {return (NULL != dynamic_cast<constant_option_c *>(option));}
bool constant_folding_c::is_retain  (symbol_c *option) {return (NULL != dynamic_cast<  retain_option_c *>(option));}

/* | var1_list ',' variable_name */
//SYM_LIST(var1_list_c)           // Not needed!

/* spec_init is one of the following...
 *    simple_spec_init_c *
 *    subrange_spec_init_c *
 *    enumerated_spec_init_c *
 */
// SYM_REF2(var1_init_decl_c, var1_list, spec_init)
void *constant_folding_c::visit(var1_init_decl_c *symbol) {return handle_var_list_decl(symbol->var1_list, symbol->spec_init);}

/* | [var1_list ','] variable_name integer '..' */
/* NOTE: This is an extension to the standard!!! */
//SYM_REF2(extensible_input_parameter_c, var_name, first_index)          // Not needed!

/* var1_list ':' array_spec_init */
//SYM_REF2(array_var_init_decl_c, var1_list, array_spec_init)            // We do not yet handle arrays!

/*  var1_list ':' initialized_structure */
//SYM_REF2(structured_var_init_decl_c, var1_list, initialized_structure) // We do not yet handle structures!

/* fb_name_list ':' function_block_type_name ASSIGN structure_initialization */
//SYM_REF2(fb_name_decl_c, fb_name_list, fb_spec_init)                   // We do not yet handle FBs!

/* fb_name_list ',' fb_name */
//SYM_LIST(fb_name_list_c)                                               // Not needed!

/* VAR_INPUT [option] input_declaration_list END_VAR */
/* option -> the RETAIN/NON_RETAIN/<NULL> directive... */
//SYM_REF3(input_declarations_c, option, input_declaration_list, method) // Not needed since we inherit from iterator_visitor_c!
// NOTE: Input variables can take any initial value, so we can not set the const_value annotation => we set fixed_init_value to false !!!
//       We must still visit it iteratively, to set the const_value of all literals in the type declarations.
void *constant_folding_c::visit(input_declarations_c *symbol) {return handle_var_decl(symbol->input_declaration_list, false);}

/* helper symbol for input_declarations */
//SYM_LIST(input_declaration_list_c)                                     // Not needed!

/* VAR_OUTPUT [RETAIN | NON_RETAIN] var_init_decl_list END_VAR */
/* option -> may be NULL ! */
//SYM_REF3(output_declarations_c, option, var_init_decl_list, method) 
void *constant_folding_c::visit(output_declarations_c *symbol) {return handle_var_decl(symbol->var_init_decl_list, !is_retain(symbol->option) && function_pou_);}

/*  VAR_IN_OUT var_declaration_list END_VAR */
//SYM_REF1(input_output_declarations_c, var_declaration_list)
// NOTE: Input variables can take any initial value, so we can not set the const_value annotation => we set fixed_init_value to false !!!
//       We must still visit it iteratively, to set the const_value of all literals in the type declarations.
void *constant_folding_c::visit(input_output_declarations_c *symbol) {return handle_var_decl(symbol->var_declaration_list, false);}

/* helper symbol for input_output_declarations */
/* var_declaration_list var_declaration ';' */
//SYM_LIST(var_declaration_list_c)                                       // Not needed since we inherit from iterator_visitor_c!

/*  var1_list ':' array_specification */
//SYM_REF2(array_var_declaration_c, var1_list, array_specification)      // We do not yet handle arrays! 

/*  var1_list ':' structure_type_name */
//SYM_REF2(structured_var_declaration_c, var1_list, structure_type_name) // We do not yet handle structures!

/* VAR [CONSTANT] var_init_decl_list END_VAR */
/* option -> may be NULL ! */
//SYM_REF2(var_declarations_c, option, var_init_decl_list)
void *constant_folding_c::visit(var_declarations_c *symbol) {return handle_var_decl(symbol->var_init_decl_list, false);}

/*  VAR RETAIN var_init_decl_list END_VAR */
//SYM_REF1(retentive_var_declarations_c, var_init_decl_list)             // Not needed since we inherit from iterator_visitor_c!
// NOTE: Retentive variables can take any initial value, so we can not set the const_value annotation => we set fixed_init_value to false !!!
//       We must still visit it iteratively, to set the const_value of all literals in the type declarations.
void *constant_folding_c::visit(retentive_var_declarations_c *symbol) {return handle_var_decl(symbol->var_init_decl_list, false);}

#if 0  
// TODO
/*  VAR [CONSTANT|RETAIN|NON_RETAIN] located_var_decl_list END_VAR */
/* option -> may be NULL ! */
SYM_REF2(located_var_declarations_c, option, located_var_decl_list)
/* helper symbol for located_var_declarations */
/* located_var_decl_list located_var_decl ';' */
SYM_LIST(located_var_decl_list_c)
/*  [variable_name] location ':' located_var_spec_init */
/* variable_name -> may be NULL ! */
SYM_REF3(located_var_decl_c, variable_name, location, located_var_spec_init)
#endif

/*| VAR_EXTERNAL [CONSTANT] external_declaration_list END_VAR */
/* option -> may be NULL ! */
// SYM_REF2(external_var_declarations_c, option, external_declaration_list)
void *constant_folding_c::visit(external_var_declarations_c *symbol) {return handle_var_decl(symbol->external_declaration_list, is_constant(symbol->option));}

/* helper symbol for external_var_declarations */
/*| external_declaration_list external_declaration';' */
// SYM_LIST(external_declaration_list_c)
// void *constant_folding_c::visit(external_declaration_list_c *symbol) {} // Not needed: we inherit from iterator_c

/*  global_var_name ':' (simple_specification|subrange_specification|enumerated_specification|array_specification|prev_declared_structure_type_name|function_block_type_name */
//SYM_REF2(external_declaration_c, global_var_name, specification)
void *constant_folding_c::visit(external_declaration_c *symbol) {
  // Note that specification->const_value will have been set by handle_var_extern_global_pair, which is called from declaration_check_c
  symbol->global_var_name->const_value = symbol->specification->const_value;
  if (fixed_init_value_) {
    std::string varName = get_var_name_c::get_name(symbol->global_var_name)->value;
    values[varName] = symbol->specification->const_value;
  }
  // If the datatype specification is a subrange or array, do constant folding of all the literals in that type declaration... (ex: literals in array subrange limits)
  symbol->specification->accept(*this);  // should never get to change the const_value of the symbol->specification symbol (only its children!).
  return NULL;
}

/* Visitors related to GLOBAL variables are not really needed, 
 * as they are already handled by handle_var_extern_global_pair, which is called from declaration_check_c 
 * 
 * This is done like this because we need to know the pairing of external<->global variables to get the cvalue
 * from the global variable. Since the external<->global pairing information is available in the declaration_check_c,
 * we have that class call the constant_folding_c::handle_var_extern_global_pair(), which will actually do the 
 * constant folding of the global and the external variable declarations!
 */
/*| VAR_GLOBAL [CONSTANT|RETAIN] global_var_decl_list END_VAR */
/* option -> may be NULL ! */
// SYM_REF2(global_var_declarations_c, option, global_var_decl_list)
/* helper symbol for global_var_declarations */
/*| global_var_decl_list global_var_decl ';' */
// SYM_LIST(global_var_decl_list_c)
/*| global_var_spec ':' [located_var_spec_init|function_block_type_name] */
/* type_specification ->may be NULL ! */
//SYM_REF2(global_var_decl_c, global_var_spec, type_specification)
/*| global_var_name location */
//SYM_REF2(global_var_spec_c, global_var_name, location)
/*  AT direct_variable */
//SYM_REF1(location_c, direct_variable)
/*| global_var_list ',' global_var_name */
//SYM_LIST(global_var_list_c)


#if 0  
// TODO
/*  var1_list ':' single_byte_string_spec */
SYM_REF2(single_byte_string_var_declaration_c, var1_list, single_byte_string_spec)
/*  STRING ['[' integer ']'] [ASSIGN single_byte_character_string] */
/* integer ->may be NULL ! */
/* single_byte_character_string ->may be NULL ! */
SYM_REF2(single_byte_string_spec_c, string_spec, single_byte_character_string)
/*   STRING ['[' integer ']'] */
/* integer ->may be NULL ! */
SYM_REF2(single_byte_limited_len_string_spec_c, string_type_name, character_string_len)
/*  WSTRING ['[' integer ']'] */
/* integer ->may be NULL ! */
SYM_REF2(double_byte_limited_len_string_spec_c, string_type_name, character_string_len)
/*  var1_list ':' double_byte_string_spec */
SYM_REF2(double_byte_string_var_declaration_c, var1_list, double_byte_string_spec)
/*  WSTRING ['[' integer ']'] [ASSIGN double_byte_character_string] */
/* integer ->may be NULL ! */
/* double_byte_character_string ->may be NULL ! */
SYM_REF2(double_byte_string_spec_c, string_spec, double_byte_character_string)
/*| VAR [RETAIN|NON_RETAIN] incompl_located_var_decl_list END_VAR */
/* option ->may be NULL ! */
SYM_REF2(incompl_located_var_declarations_c, option, incompl_located_var_decl_list)
/* helper symbol for incompl_located_var_declarations */
/*| incompl_located_var_decl_list incompl_located_var_decl ';' */
SYM_LIST(incompl_located_var_decl_list_c)
/*  variable_name incompl_location ':' var_spec */
SYM_REF3(incompl_located_var_decl_c, variable_name, incompl_location, var_spec)
/*  AT incompl_location_token */
SYM_TOKEN(incompl_location_c)
/* intermediate helper symbol for:
 *  - non_retentive_var_decls
 *  - output_declarations
 */
SYM_LIST(var_init_decl_list_c)
#endif


/***********************/
/* B 1.5.1 - Functions */
/***********************/
/* enumvalue_symtable is filled in by enum_declaration_check_c, during stage3 semantic verification, with a list of all enumerated constants declared inside this POU */
//SYM_REF4(function_declaration_c, derived_function_name, type_name, var_declarations_list, function_body, enumvalue_symtable_t enumvalue_symtable;)
void *constant_folding_c::visit(function_declaration_c *symbol) {
	values.clear(); /* Clear global map */
	/* Add initial value of all declared variables into Values map. */
	function_pou_ = true;
	symbol->var_declarations_list->accept(*this);
	function_pou_ = false;
	symbol->function_body->accept(*this);
	return NULL;
}


/* intermediate helper symbol for
 * - function_declaration
 * - function_block_declaration
 * - program_declaration
 */
// SYM_LIST(var_declarations_list_c) // Not needed since we inherit from iterator_c

/* option -> storage method, CONSTANT or <null> */
// SYM_REF2(function_var_decls_c, option, decl_list)
// NOTE: function_var_decls_c is only used inside Functions, so it is safe to call with fixed_init_value_ = true 
void *constant_folding_c::visit(function_var_decls_c *symbol) {return handle_var_decl(symbol->decl_list, true);}

/* intermediate helper symbol for function_var_decls */
// SYM_LIST(var2_init_decl_list_c) // Not needed since we inherit from iterator_c


/*****************************/
/* B 1.5.2 - Function Blocks */
/*****************************/
/*  FUNCTION_BLOCK derived_function_block_name io_OR_other_var_declarations function_block_body END_FUNCTION_BLOCK */
/* enumvalue_symtable is filled in by enum_declaration_check_c, during stage3 semantic verification, with a list of all enumerated constants declared inside this POU */
//SYM_REF3(function_block_declaration_c, fblock_name, var_declarations, fblock_body, enumvalue_symtable_t enumvalue_symtable;)
void *constant_folding_c::visit(function_block_declaration_c *symbol) {
	values.clear(); /* Clear global map */
	/* Add initial value of all declared variables into Values map. */
	function_pou_ = false;
	symbol->var_declarations->accept(*this);
	symbol->fblock_body->accept(*this);
	return NULL;
}

/*  VAR_TEMP temp_var_decl_list END_VAR */
// SYM_REF1(temp_var_decls_c, var_decl_list)
void *constant_folding_c::visit(temp_var_decls_c *symbol) {debug_c::print(symbol); return handle_var_decl(symbol->var_decl_list, true);}

/* intermediate helper symbol for temp_var_decls */
// SYM_LIST(temp_var_decls_list_c)

/*  VAR NON_RETAIN var_init_decl_list END_VAR */
// SYM_REF1(non_retentive_var_decls_c, var_decl_list)
// NOTE: non_retentive_var_decls_c is only used inside FBs and Programs, so it is safe to call with fixed_init_value_ = false 
void *constant_folding_c::visit(non_retentive_var_decls_c *symbol) {debug_c::print(symbol); return handle_var_decl(symbol->var_decl_list, false);}


/**********************/
/* B 1.5.3 - Programs */
/**********************/
/*  PROGRAM program_type_name program_var_declarations_list function_block_body END_PROGRAM */
//SYM_REF3(program_declaration_c, program_type_name, var_declarations, function_block_body, enumvalue_symtable_t enumvalue_symtable;)
void *constant_folding_c::visit(program_declaration_c *symbol) {
	values.clear(); /* Clear global map */
	/* Add initial value of all declared variables into Values map. */
	function_pou_ = false;
	symbol->var_declarations->accept(*this);
	symbol->function_block_body->accept(*this);
	return NULL;
}


/****************************************/
/* B.2 - Language IL (Instruction List) */
/****************************************/
/***********************************/
/* B 2.1 Instructions and Operands */
/***********************************/
/* Not needed, since we inherit from iterator_visitor_c */
/*| instruction_list il_instruction */
// SYM_LIST(instruction_list_c)
// void *constant_folding_c::visit(instruction_list_c *symbol) {}

/* | label ':' [il_incomplete_instruction] eol_list */
// SYM_REF2(il_instruction_c, label, il_instruction)
// void *visit(instruction_list_c *symbol);
void *constant_folding_c::visit(il_instruction_c *symbol) {
	if (NULL == symbol->il_instruction) {
		/* This empty/null il_instruction does not change the value of the current/default IL variable.
		 * So it inherits the candidate_datatypes from it's previous IL instructions!
		 */
		intersect_prev_cvalues(symbol);
	} else {
		il_instruction_c fake_prev_il_instruction = *symbol;
		intersect_prev_cvalues(&fake_prev_il_instruction);

		if (symbol->prev_il_instruction.size() == 0)  prev_il_instruction = NULL;
		else                                          prev_il_instruction = &fake_prev_il_instruction;
		symbol->il_instruction->accept(*this);
		prev_il_instruction = NULL;

		/* This object has (inherits) the same cvalues as the il_instruction */
		symbol->const_value = symbol->il_instruction->const_value;
	}

	return NULL;
}


void *constant_folding_c::visit(il_simple_operation_c *symbol) {
	/* determine the cvalue of the operand */
	if (NULL != symbol->il_operand) {
		symbol->il_operand->accept(*this);
	}
	/* determine the cvalue resulting from executing the il_operator... */
	il_operand = symbol->il_operand;
	symbol->il_simple_operator->accept(*this);
	il_operand = NULL;
	/* This object has (inherits) the same cvalues as the il_instruction */
	symbol->const_value = symbol->il_simple_operator->const_value;
	return NULL;
}


/* TODO: handle function invocations... */
/* | function_name [il_operand_list] */
/* NOTE: The parameters 'called_function_declaration' and 'extensible_param_count' are used to pass data between the stage 3 and stage 4. */
// SYM_REF2(il_function_call_c, function_name, il_operand_list, symbol_c *called_function_declaration; int extensible_param_count;)
// void *constant_folding_c::visit(il_function_call_c *symbol) {}


/* | il_expr_operator '(' [il_operand] eol_list [simple_instr_list] ')' */
// SYM_REF3(il_expression_c, il_expr_operator, il_operand, simple_instr_list);
void *constant_folding_c::visit(il_expression_c *symbol) {
  symbol_c *prev_il_instruction_backup = prev_il_instruction;
  
  /* Stage2 will insert an artificial (and equivalent) LD <il_operand> to the simple_instr_list if necessary. We can therefore ignore the 'il_operand' entry! */
  // if (NULL != symbol->il_operand)
  //   symbol->il_operand->accept(*this);

  if(symbol->simple_instr_list != NULL)
    symbol->simple_instr_list->accept(*this);

  /* Now do the operation,  */
  il_operand = symbol->simple_instr_list;
  prev_il_instruction = prev_il_instruction_backup;
  symbol->il_expr_operator->accept(*this);
  il_operand = NULL;
  
  /* This object has (inherits) the same cvalues as the il_instruction */
  symbol->const_value = symbol->il_expr_operator->const_value;
  
  /* Since stage2 will insert an artificial (and equivalent) LD <il_operand> to the simple_instr_list when an 'il_operand' exists, we know
   * that if (symbol->il_operand != NULL), then the first IL instruction in the simple_instr_list will be the equivalent and artificial
   * 'LD <il_operand>' IL instruction.
   * Just to be cosistent, we will copy the constant info back into the il_operand, even though this should not be necessary!
   */
  if ((NULL != symbol->il_operand) && ((NULL == symbol->simple_instr_list) || (0 == ((list_c *)symbol->simple_instr_list)->n))) ERROR; // stage2 is not behaving as we expect it to!
  if  (NULL != symbol->il_operand)
    symbol->il_operand->const_value = ((list_c *)symbol->simple_instr_list)->elements[0]->const_value;

  return NULL;
}



void *constant_folding_c::visit(il_jump_operation_c *symbol) {
  /* recursive call to fill const values... */
  il_operand = NULL;
  symbol->il_jump_operator->accept(*this);
  il_operand = NULL;
  /* This object has (inherits) the same cvalues as the il_jump_operator */
  symbol->const_value = symbol->il_jump_operator->const_value;
  return NULL;
}



/* FB calls leave the value in the accumulator unchanged */
/*   il_call_operator prev_declared_fb_name
 * | il_call_operator prev_declared_fb_name '(' ')'
 * | il_call_operator prev_declared_fb_name '(' eol_list ')'
 * | il_call_operator prev_declared_fb_name '(' il_operand_list ')'
 * | il_call_operator prev_declared_fb_name '(' eol_list il_param_list ')'
 */
/* NOTE: The parameter 'called_fb_declaration'is used to pass data between stage 3 and stage4 (although currently it is not used in stage 4 */
// SYM_REF4(il_fb_call_c, il_call_operator, fb_name, il_operand_list, il_param_list, symbol_c *called_fb_declaration)
void *constant_folding_c::visit(il_fb_call_c *symbol) {return handle_move(symbol, prev_il_instruction);}


/* TODO: handle function invocations... */
/* | function_name '(' eol_list [il_param_list] ')' */
/* NOTE: The parameter 'called_function_declaration' is used to pass data between the stage 3 and stage 4. */
// SYM_REF2(il_formal_funct_call_c, function_name, il_param_list, symbol_c *called_function_declaration; int extensible_param_count;)
// void *constant_folding_c::visit(il_formal_funct_call_c *symbol) {return NULL;}



/* Not needed, since we inherit from iterator_visitor_c */
//  void *constant_folding_c::visit(il_operand_list_c *symbol);



/* | simple_instr_list il_simple_instruction */
/* This object is referenced by il_expression_c objects */
void *constant_folding_c::visit(simple_instr_list_c *symbol) {
  if (symbol->n <= 0)
    return NULL;  /* List is empty! Nothing to do. */
    
  for(int i = 0; i < symbol->n; i++)
    symbol->elements[i]->accept(*this);

  /* This object has (inherits) the same cvalues as the il_jump_operator */
  symbol->const_value = symbol->elements[symbol->n-1]->const_value;
  return NULL;
}



// SYM_REF1(il_simple_instruction_c, il_simple_instruction, symbol_c *prev_il_instruction;)
void *constant_folding_c::visit(il_simple_instruction_c *symbol) {
  if (symbol->prev_il_instruction.size() > 1) ERROR; /* There should be no labeled insructions inside an IL expression! */
  if (symbol->prev_il_instruction.size() == 0)  prev_il_instruction = NULL;
  else                                          prev_il_instruction = symbol->prev_il_instruction[0];
  symbol->il_simple_instruction->accept(*this);
  prev_il_instruction = NULL;

  /* This object has (inherits) the same cvalues as the il_jump_operator */
  symbol->const_value = symbol->il_simple_instruction->const_value;
  return NULL;
}


/*
    void *visit(il_param_list_c *symbol);
    void *visit(il_param_assignment_c *symbol);
    void *visit(il_param_out_assignment_c *symbol);
*/


/*******************/
/* B 2.2 Operators */
/*******************/
void *constant_folding_c::visit(   LD_operator_c *symbol) {return handle_move(symbol, il_operand);}
void *constant_folding_c::visit(  LDN_operator_c *symbol) {return handle_not (symbol, il_operand);}

/* NOTE: we are implementing a constant folding algorithm, not a constant propagation algorithm.
 *       For the constant propagation algorithm, the correct implementation of ST(N)_operator_c would be...
 */
//void *constant_folding_c::visit(   ST_operator_c *symbol) {return handle_move(il_operand, symbol);}
//void *constant_folding_c::visit(  STN_operator_c *symbol) {return handle_not (il_operand, symbol);}
void *constant_folding_c::visit(   ST_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(  STN_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}

/* NOTE: the standard allows syntax in which the NOT operator is followed by an optional <il_operand>
 *              NOT [<il_operand>]
 *       However, it does not define the semantic of the NOT operation when the <il_operand> is specified.
 *       We therefore consider it an error if an il_operand is specified! This error will be caught elsewhere!
 */
void *constant_folding_c::visit(  NOT_operator_c *symbol) {return handle_not(symbol, prev_il_instruction);}

/* NOTE: Since we are only implementing a constant folding algorithm, and not a constant propagation algorithm,
 *       the following IL instructions do not change/set the value of the il_operand!
 */
void *constant_folding_c::visit(    S_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(    R_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}

/* FB calls leave the value in the accumulator unchanged */
void *constant_folding_c::visit(   S1_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   R1_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(  CLK_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   CU_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   CD_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   PV_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   IN_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(   PT_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}

void *constant_folding_c::visit(  AND_operator_c *symbol) {return handle_and (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(   OR_operator_c *symbol) {return handle_or  (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(  XOR_operator_c *symbol) {return handle_xor (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit( ANDN_operator_c *symbol) {       handle_and (symbol, prev_il_instruction, il_operand); return handle_not(symbol, symbol);}
void *constant_folding_c::visit(  ORN_operator_c *symbol) {       handle_or  (symbol, prev_il_instruction, il_operand); return handle_not(symbol, symbol);}
void *constant_folding_c::visit( XORN_operator_c *symbol) {       handle_xor (symbol, prev_il_instruction, il_operand); return handle_not(symbol, symbol);}

void *constant_folding_c::visit(  ADD_operator_c *symbol) {return handle_add (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(  SUB_operator_c *symbol) {return handle_sub (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(  MUL_operator_c *symbol) {return handle_mul (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(  DIV_operator_c *symbol) {return handle_div (symbol, prev_il_instruction, il_operand);}
void *constant_folding_c::visit(  MOD_operator_c *symbol) {return handle_mod (symbol, prev_il_instruction, il_operand);}

void *constant_folding_c::visit(   GT_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, > );}
void *constant_folding_c::visit(   GE_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, >=);}
void *constant_folding_c::visit(   EQ_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, ==);}
void *constant_folding_c::visit(   LT_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, < );}
void *constant_folding_c::visit(   LE_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, <=);}
void *constant_folding_c::visit(   NE_operator_c *symbol) {       handle_cmp (symbol, prev_il_instruction, il_operand, !=);}

void *constant_folding_c::visit(  CAL_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(  RET_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(  JMP_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit( CALC_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(CALCN_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit( RETC_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(RETCN_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit( JMPC_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}
void *constant_folding_c::visit(JMPCN_operator_c *symbol) {return handle_move(symbol, prev_il_instruction);}




/***************************************/
/* B.3 - Language ST (Structured Text) */
/***************************************/
/***********************/
/* B 3.1 - Expressions */
/***********************/
void *constant_folding_c::visit(    or_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_or (symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   xor_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_xor(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   and_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_and(symbol, symbol->l_exp, symbol->r_exp);}

void *constant_folding_c::visit(   equ_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, ==);}
void *constant_folding_c::visit(notequ_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, !=);}
void *constant_folding_c::visit(    lt_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, < );}
void *constant_folding_c::visit(    gt_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, > );}
void *constant_folding_c::visit(    le_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, <=);}
void *constant_folding_c::visit(    ge_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this);        handle_cmp (symbol, symbol->l_exp, symbol->r_exp, >=);}

void *constant_folding_c::visit(   add_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_add(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   sub_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_sub(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   mul_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_mul(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   div_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_div(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit(   mod_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_mod(symbol, symbol->l_exp, symbol->r_exp);}
void *constant_folding_c::visit( power_expression_c *symbol) {symbol->l_exp->accept(*this); symbol->r_exp->accept(*this); return handle_pow(symbol, symbol->l_exp, symbol->r_exp);}

void *constant_folding_c::visit(   neg_expression_c *symbol) {symbol->  exp->accept(*this); return handle_neg(symbol, symbol->exp);}
void *constant_folding_c::visit(   not_expression_c *symbol) {symbol->  exp->accept(*this); return handle_not(symbol, symbol->exp);}

/* TODO: handle function invocations... */
// void *fill_candidate_datatypes_c::visit(function_invocation_c *symbol) {}



/*********************************/
/* B 3.2.1 Assignment Statements */
/*********************************/
void *constant_folding_c::visit(assignment_statement_c *symbol) {
	std::string varName;

	symbol->r_exp->accept(*this);
	symbol->l_exp->accept(*this); // if the lvalue has an array, do contant folding of the array indexes!
	symbol->l_exp->const_value = symbol->r_exp->const_value;
	varName = get_var_name_c::get_name(symbol->l_exp)->value;
	values[varName] = symbol->l_exp->const_value;
	return NULL;
}

#if DO_CONSTANT_PROPAGATION__
/********************************/
/* B 3.2.3 Selection Statements */
/********************************/
void *constant_folding_c::visit(if_statement_c *symbol) {
	map_values_t values_incoming;
	map_values_t values_statement_result;
	map_values_t values_elsestatement_result;
	map_values_t::iterator itr;

	/* Optimize dead code */
	symbol->expression->accept(*this);
	if (VALID_CVALUE(bool, symbol->expression) && GET_CVALUE(bool, symbol->expression) == false)
		return NULL;

	values_incoming = values; /* save incoming status */
	symbol->statement_list->accept(*this);
	values_statement_result = values;
	if (NULL != symbol->else_statement_list) {
		values = values_incoming;
		symbol->else_statement_list->accept(*this);
		values_elsestatement_result = values;
	} else
		values_elsestatement_result = values_incoming;
	values = inner_left_join_values(values_statement_result, values_elsestatement_result);

	return NULL;
}

/********************************/
/* B 3.2.4 Iteration Statements */
/********************************/
void *constant_folding_c::visit(for_statement_c *symbol) {
	map_values_t values_incoming;
	map_values_t values_statement_result;
	std::string varName;

	values_incoming = values; /* save incoming status */
	symbol->beg_expression->accept(*this);
	symbol->end_expression->accept(*this);
	varName =  get_var_name_c::get_name(symbol->control_variable)->value;
	values[varName]._int64.status = const_value_c::cs_non_const;

	/* Optimize dead code */
	if (NULL != symbol->by_expression) {
		symbol->by_expression->accept(*this);
		if (VALID_CVALUE(int64, symbol->by_expression ) &&   GET_CVALUE(int64, symbol->by_expression ) > 0 &&
			VALID_CVALUE(int64, symbol->beg_expression) && VALID_CVALUE(int64, symbol->end_expression)     &&
			  GET_CVALUE(int64, symbol->beg_expression) >    GET_CVALUE(int64, symbol->end_expression))
			return NULL;

		if (VALID_CVALUE(int64, symbol->by_expression ) &&   GET_CVALUE(int64, symbol->by_expression ) < 0 &&
			VALID_CVALUE(int64, symbol->beg_expression) && VALID_CVALUE(int64, symbol->end_expression)    &&
			  GET_CVALUE(int64, symbol->beg_expression) <    GET_CVALUE(int64, symbol->end_expression))
			return NULL;

	} else {
		if (VALID_CVALUE(int64, symbol->beg_expression) && VALID_CVALUE(int64, symbol->end_expression)     &&
			  GET_CVALUE(int64, symbol->beg_expression) >    GET_CVALUE(int64, symbol->end_expression))
			return NULL;

	}


	symbol->statement_list->accept(*this);
	values_statement_result = values;
	values = inner_left_join_values(values_statement_result, values_incoming);

	return NULL;
}

void *constant_folding_c::visit(while_statement_c *symbol) {
	map_values_t values_incoming;
	map_values_t values_statement_result;

	/* Optimize dead code */
	symbol->expression->accept(*this);
	if (VALID_CVALUE(bool, symbol->expression) && GET_CVALUE(bool, symbol->expression) == false)
		return NULL;

	values_incoming = values; /* save incoming status */
	symbol->statement_list->accept(*this);
	values_statement_result = values;
	values = inner_left_join_values(values_statement_result, values_incoming);

	return NULL;
}

void *constant_folding_c::visit(repeat_statement_c *symbol) {
	map_values_t values_incoming;
	map_values_t values_statement_result;

	values_incoming = values; /* save incoming status */
	symbol->statement_list->accept(*this);

	/* Optimize dead code */
	symbol->expression->accept(*this);
	if (VALID_CVALUE(bool, symbol->expression) && GET_CVALUE(bool, symbol->expression) == true)
		return NULL;

	values_statement_result = values;
	values = inner_left_join_values(values_statement_result, values_incoming);

	return NULL;
}

#endif  // DO_CONSTANT_PROPAGATION__







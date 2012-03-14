/*
 *  matiec - a compiler for the programming languages defined in IEC 61131-3
 *
 *  Copyright (C) 2009-2011  Mario de Sousa (msousa@fe.up.pt)
 *  Copyright (C) 20011-2012 Manuele Conti (manuele.conti@sirius-es.it)
 *  Copyright (C) 20011-2012 Matteo Facchinetti (matteo.facchinetti@sirius-es.it)
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


/*
 *  Fill candidate list of data types for all symbols
 */

#include "print_datatypes_error.hh"
#include "datatype_functions.hh"

#include <typeinfo>
#include <list>
#include <string>
#include <string.h>
#include <strings.h>






#define FIRST_(symbol1, symbol2) (((symbol1)->first_order < (symbol2)->first_order)   ? (symbol1) : (symbol2))
#define  LAST_(symbol1, symbol2) (((symbol1)->last_order  > (symbol2)->last_order)    ? (symbol1) : (symbol2))

#define STAGE3_ERROR(error_level, symbol1, symbol2, ...) {                                                                  \
  if (current_display_error_level >= error_level) {                                                                         \
    fprintf(stderr, "%s:%d-%d..%d-%d: error: ",                                                                             \
            FIRST_(symbol1,symbol2)->first_file, FIRST_(symbol1,symbol2)->first_line, FIRST_(symbol1,symbol2)->first_column,\
                                                 LAST_(symbol1,symbol2) ->last_line,  LAST_(symbol1,symbol2) ->last_column);\
    fprintf(stderr, __VA_ARGS__);                                                                                           \
    fprintf(stderr, "\n");                                                                                                  \
    il_error = true;                                                                                                        \
    error_found = true;                                                                                                     \
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


/* set to 1 to see debug info during execution */
static int debug = 0;

print_datatypes_error_c::print_datatypes_error_c(symbol_c *ignore) {
	error_found = false;
	warning_found = false;
	current_display_error_level = error_level_default;
}

print_datatypes_error_c::~print_datatypes_error_c(void) {
}

int print_datatypes_error_c::get_error_found() {
	return error_found;
}





/* Verify if the datatypes of all prev_il_instructions are valid and equal!  */
static bool are_all_datatypes_of_prev_il_instructions_datatypes_equal(il_instruction_c *symbol) {
	if (NULL == symbol) ERROR;
	bool res;
	
	if (symbol->prev_il_instruction.size() > 0)
		res = is_type_valid(symbol->prev_il_instruction[0]->datatype);

	for (unsigned int i = 1; i < symbol->prev_il_instruction.size(); i++)
		res &= is_type_equal(symbol->prev_il_instruction[i-1]->datatype, symbol->prev_il_instruction[i]->datatype);
	
	return res;
}




/* a helper function... */
symbol_c *print_datatypes_error_c::base_type(symbol_c *symbol) {
	/* NOTE: symbol == NULL is valid. It will occur when, for e.g., an undefined/undeclared symbolic_variable is used
	 *       in the code.
	 */
	if (symbol == NULL) return NULL;
	return (symbol_c *)symbol->accept(search_base_type);
}



/*
typedef struct {
  symbol_c *function_name,
  symbol_c *nonformal_operand_list,
  symbol_c *   formal_operand_list,

  std::vector <symbol_c *> &candidate_functions,  
  symbol_c &*called_function_declaration,
  int      &extensible_param_count
} generic_function_call_t;
*/
void print_datatypes_error_c::handle_function_invocation(symbol_c *fcall, generic_function_call_t fcall_data) {
	symbol_c *param_value, *param_name;
	function_call_param_iterator_c fcp_iterator(fcall);
	bool function_invocation_error = false;
	const char *POU_str = NULL;

	if (generic_function_call_t::POU_FB       == fcall_data.POU_type)  POU_str = "FB";
	if (generic_function_call_t::POU_function == fcall_data.POU_type)  POU_str = "function";
	if (NULL == POU_str) ERROR;

	if ((NULL != fcall_data.formal_operand_list) && (NULL != fcall_data.nonformal_operand_list)) 
		ERROR;

	symbol_c *f_decl = fcall_data.called_function_declaration;
	if ((NULL == f_decl) && (generic_function_call_t::POU_FB ==fcall_data.POU_type)) {
		/* Due to the way the syntax analysis is buit (i.e. stage 2), this should never occur. */
		/* I.e., a FB invocation using an undefined FB variable is not possible in the current implementation of stage 2. */
		ERROR;
	}
	if (NULL == f_decl) {
		/* we now try to find any function declaration with the same name, just so we can provide some relevant error messages */
		function_symtable_t::iterator lower = function_symtable.lower_bound(fcall_data.function_name);
		if (lower == function_symtable.end()) ERROR;
		f_decl = function_symtable.get_value(lower);
	}

	if (NULL != fcall_data.formal_operand_list) {
		fcall_data.formal_operand_list->accept(*this);
		if (NULL != f_decl) {
			function_param_iterator_c fp_iterator(f_decl);
			while ((param_name = fcp_iterator.next_f()) != NULL) {
				param_value = fcp_iterator.get_current_value();

				/* Check if there are duplicate parameter values */
				if(fcp_iterator.search_f(param_name) != param_value) {
					function_invocation_error = true;
					STAGE3_ERROR(0, param_name, param_name, "Duplicate parameter '%s' when invoking %s '%s'", ((identifier_c *)param_name)->value, POU_str, ((identifier_c *)fcall_data.function_name)->value);
					continue; /* jump to next parameter */
				}

				/* Find the corresponding parameter in function declaration */
				if (NULL == fp_iterator.search(param_name)) {
					function_invocation_error = true;
					STAGE3_ERROR(0, param_name, param_name, "Invalid parameter '%s' when invoking %s '%s'", ((identifier_c *)param_name)->value, POU_str, ((identifier_c *)fcall_data.function_name)->value);
					continue; /* jump to next parameter */
				} 

				/* check whether direction (IN, OUT, IN_OUT) and assignment types (:= , =>) are compatible !!! */
				/* Obtaining the assignment direction:  := (assign_in) or => (assign_out) */
				function_call_param_iterator_c::assign_direction_t call_param_dir = fcp_iterator.get_assign_direction();
				/* Get the parameter direction: IN, OUT, IN_OUT */
				function_param_iterator_c::param_direction_t param_dir = fp_iterator.param_direction();
				if          (function_call_param_iterator_c::assign_in  == call_param_dir) {
					if ((function_param_iterator_c::direction_in    != param_dir) &&
					    (function_param_iterator_c::direction_inout != param_dir)) {
						function_invocation_error = true;
						STAGE3_ERROR(0, param_name, param_name, "Invalid assignment syntax ':=' used for parameter '%s', when invoking %s '%s'", ((identifier_c *)param_name)->value, POU_str, ((identifier_c *)fcall_data.function_name)->value);
						continue; /* jump to next parameter */
					}
				} else if   (function_call_param_iterator_c::assign_out == call_param_dir) {
					if ((function_param_iterator_c::direction_out   != param_dir)) {
						function_invocation_error = true;
						STAGE3_ERROR(0, param_name, param_name, "Invalid assignment syntax '=>' used for parameter '%s', when invoking %s '%s'", ((identifier_c *)param_name)->value, POU_str, ((identifier_c *)fcall_data.function_name)->value);
						continue; /* jump to next parameter */
					}
				} else ERROR;

				if (NULL == param_value->datatype) {
					function_invocation_error = true;
					STAGE3_ERROR(0, param_value, param_value, "Data type incompatibility between parameter '%s' and value being passed, when invoking %s '%s'", ((identifier_c *)param_name)->value, POU_str, ((identifier_c *)fcall_data.function_name)->value);
					continue; /* jump to next parameter */
				}
			}
		}
	}
	if (NULL != fcall_data.nonformal_operand_list) {
		fcall_data.nonformal_operand_list->accept(*this);
		if (f_decl)
			for (int i = 1; (param_value = fcp_iterator.next_nf()) != NULL; i++) {
		  		/* TODO: verify if it is lvalue when INOUT or OUTPUT parameters! */

				if (NULL == param_value->datatype) {
					function_invocation_error = true;
					STAGE3_ERROR(0, param_value, param_value, "Data type incompatibility for value passed in position %d when invoking %s '%s'", i, POU_str, ((identifier_c *)fcall_data.function_name)->value);
				}
			}
	}

	if (NULL == fcall_data.called_function_declaration) {
		function_invocation_error = true;
		STAGE3_ERROR(0, fcall, fcall, "Unable to resolve which overloaded %s '%s' is being invoked.", POU_str, ((identifier_c *)fcall_data.function_name)->value);
	}

	if (function_invocation_error) {
		/* No compatible function exists */
		STAGE3_ERROR(2, fcall, fcall, "Invalid parameters when invoking %s '%s'", POU_str, ((identifier_c *)fcall_data.function_name)->value);
	} 

	return;
}



void *print_datatypes_error_c::handle_implicit_il_fb_invocation(const char *param_name, symbol_c *il_operator, symbol_c *called_fb_declaration) {
	if (NULL == il_operand) {
		STAGE3_ERROR(0, il_operator, il_operator, "Missing operand for FB call operator '%s'.", param_name);
		return NULL;
	}
	il_operand->accept(*this);
	
	if (NULL == called_fb_declaration) {
		STAGE3_ERROR(0, il_operator, il_operand, "Invalid FB call: operand is not a FB instance.");
		return NULL;
	}

	if (fake_prev_il_instruction->prev_il_instruction.empty()) {
		STAGE3_ERROR(0, il_operator, il_operand, "FB invocation operator '%s' must be preceded by a 'LD' (or equivalent) operator.", param_name);	
		return NULL;
	}

	/* Find the corresponding parameter in function declaration */
	function_param_iterator_c fp_iterator(called_fb_declaration);
	if (NULL == fp_iterator.search(param_name)) {
		/* TODO: must also check whther it is an IN parameter!! */
		/* NOTE: although all standard FBs have the implicit FB calls defined as input parameters
		*        (i.e., for all standard FBs, CLK, PT, IN, CU, CD, S1, R1, etc... is always an input parameter)
		*        if a non-standard (i.e. a FB not defined in the standard library) FB is being called, then
		*        this (CLK, PT, IN, CU, ...) parameter may just have been defined as OUT or INOUT,
		*        which will not work for an implicit FB call!
		*/
		STAGE3_ERROR(0, il_operator, il_operand, "FB called by '%s' operator does not have a parameter named '%s'", param_name, param_name);	
		return NULL;
	}
	if (!are_all_datatypes_of_prev_il_instructions_datatypes_equal(fake_prev_il_instruction)) {
		STAGE3_ERROR(0, il_operator, il_operand, "Data type incompatibility between parameter '%s' and value being passed.", param_name);
		return NULL;
	}
	

	/* NOTE: The error_level currently being used for errors in variables/constants etc... is rather high.
	 *       However, in the case of an implicit FB call, if the datatype of the operand == NULL, this may be
	 *       the __only__ indication of an error! So we test it here again, to make sure thtis error will really
	 *       be printed out!
	 */
	if (NULL == il_operand->datatype) {
		/* Note: the case of (NULL == fb_declaration) was already caught above! */
// 		if (NULL != fb_declaration) {
			STAGE3_ERROR(0, il_operator, il_operator, "Invalid FB call: Datatype incompatibility between the FB's '%s' parameter and value being passed, or paramater '%s' is not a 'VAR_INPUT' parameter.", param_name, param_name);
			return NULL;
// 		}
	}

	return NULL;
}


/*********************/
/* B 1.2 - Constants */
/*********************/
/******************************/
/* B 1.2.1 - Numeric Literals */
/******************************/
void *print_datatypes_error_c::visit(real_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_REAL data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_REAL data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(integer_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_INT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(neg_real_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_REAL data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_REAL data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(neg_integer_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_INT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(binary_integer_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_INT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(octal_integer_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_INT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(hex_integer_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for ANY_INT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(integer_literal_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for %s data type.", elementary_type_c::to_string(symbol->type));
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_INT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(real_literal_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for %s data type.", elementary_type_c::to_string(symbol->type));
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_REAL data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(bit_string_literal_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for %s data type.", elementary_type_c::to_string(symbol->type));
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_BIT data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(boolean_literal_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Value is not valid for %s data type.", elementary_type_c::to_string(symbol->type));
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_BOOL data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(boolean_true_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Value is not valid for ANY_BOOL data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_BOOL data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(boolean_false_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Value is not valid for ANY_BOOL data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "ANY_BOOL data type not valid in this location.");
	}
	return NULL;
}

/*******************************/
/* B.1.2.2   Character Strings */
/*******************************/
void *print_datatypes_error_c::visit(double_byte_character_string_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for WSTRING data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "WSTRING data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(single_byte_character_string_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for STRING data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "STRING data type not valid in this location.");
	}
	return NULL;
}

/***************************/
/* B 1.2.3 - Time Literals */
/***************************/
/************************/
/* B 1.2.3.1 - Duration */
/************************/
void *print_datatypes_error_c::visit(duration_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid syntax for TIME data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "TIME data type not valid in this location.");
	}
	return NULL;
}

/************************************/
/* B 1.2.3.2 - Time of day and Date */
/************************************/
void *print_datatypes_error_c::visit(time_of_day_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid syntax for TOD data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "TOD data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(date_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid syntax for DATE data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "DATE data type not valid in this location.");
	}
	return NULL;
}

void *print_datatypes_error_c::visit(date_and_time_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid syntax for DT data type.");
	} else if (NULL == symbol->datatype) {
		STAGE3_ERROR(4, symbol, symbol, "DT data type not valid in this location.");
	}
	return NULL;
}

/**********************/
/* B 1.3 - Data types */
/**********************/
/********************************/
/* B 1.3.3 - Derived data types */
/********************************/
void *print_datatypes_error_c::visit(data_type_declaration_c *symbol) {
	// TODO !!!
	/* for the moment we must return NULL so semantic analysis of remaining code is not interrupted! */
	return NULL;
}

void *print_datatypes_error_c::visit(enumerated_value_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Ambiguous enumerate value or Variable not declared in this scope.");
	return NULL;
}


/*********************/
/* B 1.4 - Variables */
/*********************/
void *print_datatypes_error_c::visit(symbolic_variable_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Variable not declared in this scope.");
	return NULL;
}

/********************************************/
/* B 1.4.1 - Directly Represented Variables */
/********************************************/
void *print_datatypes_error_c::visit(direct_variable_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Numerical value exceeds range for located variable data type.");
	return NULL;
}

/*************************************/
/* B 1.4.2 - Multi-element variables */
/*************************************/
/*  subscripted_variable '[' subscript_list ']' */
// SYM_REF2(array_variable_c, subscripted_variable, subscript_list)
void *print_datatypes_error_c::visit(array_variable_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Array variable not declared in this scope.");
	
	/* recursively call the subscript list to print any errors in the expressions used in the subscript...*/
	symbol->subscript_list->accept(*this);
	return NULL;
}

/* subscript_list ',' subscript */
// SYM_LIST(subscript_list_c)
/* NOTE: we inherit from iterator visitor, so we do not need to implement this method... */
// void *print_datatypes_error_c::visit(subscript_list_c *symbol)


/*  record_variable '.' field_selector */
/*  WARNING: input and/or output variables of function blocks
 *           may be accessed as fields of a structured variable!
 *           Code handling a structured_variable_c must take
 *           this into account!
 */
// SYM_REF2(structured_variable_c, record_variable, field_selector)
/* NOTE: We do not recursively determine the data types of each field_selector in fill_candidate_datatypes_c,
 * so it does not make sense to recursively visit all the field_selectors to print out error messages. 
 * Maybe in the future, if we find the need to print out more detailed error messages, we might do it that way. For now, we don't!
 */
void *print_datatypes_error_c::visit(structured_variable_c *symbol) {
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Undeclared structured/FB variable.");
	return NULL;
}

/************************************/
/* B 1.5 Program organization units */
/************************************/
/*********************/
/* B 1.5.1 Functions */
/*********************/
void *print_datatypes_error_c::visit(function_declaration_c *symbol) {
	search_varfb_instance_type = new search_varfb_instance_type_c(symbol);
	/* We do not check for data type errors in variable declarations, Skip this for now... */
// 	symbol->var_declarations_list->accept(*this);
	if (debug) printf("Print error data types list in body of function %s\n", ((token_c *)(symbol->derived_function_name))->value);
	il_parenthesis_level = 0;
	il_error = false;
	symbol->function_body->accept(*this);
	delete search_varfb_instance_type;
	search_varfb_instance_type = NULL;
	return NULL;
}

/***************************/
/* B 1.5.2 Function blocks */
/***************************/
void *print_datatypes_error_c::visit(function_block_declaration_c *symbol) {
	search_varfb_instance_type = new search_varfb_instance_type_c(symbol);
	/* We do not check for data type errors in variable declarations, Skip this for now... */
// 	symbol->var_declarations->accept(*this);
	if (debug) printf("Print error data types list in body of FB %s\n", ((token_c *)(symbol->fblock_name))->value);
	il_parenthesis_level = 0;
	il_error = false;
	symbol->fblock_body->accept(*this);
	delete search_varfb_instance_type;
	search_varfb_instance_type = NULL;
	return NULL;
}

/**********************/
/* B 1.5.3 - Programs */
/**********************/
void *print_datatypes_error_c::visit(program_declaration_c *symbol) {
	search_varfb_instance_type = new search_varfb_instance_type_c(symbol);
	/* We do not check for data type errors in variable declarations, Skip this for now... */
// 	symbol->var_declarations->accept(*this);
	if (debug) printf("Print error data types list in body of program %s\n", ((token_c *)(symbol->program_type_name))->value);
	il_parenthesis_level = 0;
	il_error = false;
	symbol->function_block_body->accept(*this);
	delete search_varfb_instance_type;
	search_varfb_instance_type = NULL;
	return NULL;
}



/********************************/
/* B 1.7 Configuration elements */
/********************************/
void *print_datatypes_error_c::visit(configuration_declaration_c *symbol) {
	// TODO !!!
	/* for the moment we must return NULL so semantic analysis of remaining code is not interrupted! */
	return NULL;
}

/****************************************/
/* B.2 - Language IL (Instruction List) */
/****************************************/
/***********************************/
/* B 2.1 Instructions and Operands */
/***********************************/

// void *visit(instruction_list_c *symbol);

/* | label ':' [il_incomplete_instruction] eol_list */
// SYM_REF2(il_instruction_c, label, il_instruction)
void *print_datatypes_error_c::visit(il_instruction_c *symbol) {
	if (NULL != symbol->il_instruction) {
// #if 0
		il_instruction_c tmp_prev_il_instruction(NULL, NULL);
		/* the narrow algorithm will need access to the intersected candidate_datatype lists of all prev_il_instructions, as well as the 
		 * list of the prev_il_instructions.
		 * Instead of creating two 'global' (within the class) variables, we create a single il_instruction_c variable (fake_prev_il_instruction),
		 * and shove that data into this single variable.
		 */
		tmp_prev_il_instruction.prev_il_instruction = symbol->prev_il_instruction;
		intersect_prev_candidate_datatype_lists(&tmp_prev_il_instruction);
		if (are_all_datatypes_of_prev_il_instructions_datatypes_equal(symbol))
			if (symbol->prev_il_instruction.size() > 0)
				tmp_prev_il_instruction.datatype = (symbol->prev_il_instruction[0])->datatype;
		/* Tell the il_instruction the datatype that it must generate - this was chosen by the next il_instruction (remember: we are iterating backwards!) */
		fake_prev_il_instruction = &tmp_prev_il_instruction;
		symbol->il_instruction->accept(*this);
		fake_prev_il_instruction = NULL;
// #endif
// 		if (symbol->prev_il_instruction.size() > 1) ERROR; /* only valid for now! */
// 		if (symbol->prev_il_instruction.size() == 0)  prev_il_instruction = NULL;
// 		else                                          prev_il_instruction = symbol->prev_il_instruction[0];

// 		symbol->il_instruction->accept(*this);
// 		prev_il_instruction = NULL;
	}

	return NULL;
}



void *print_datatypes_error_c::visit(il_simple_operation_c *symbol) {
	il_operand = symbol->il_operand;
	if (NULL != symbol->il_operand) {
		symbol->il_operand->accept(*this);
	}
	/* recursive call to see whether data types are compatible */
	symbol->il_simple_operator->accept(*this);
	il_operand = NULL;
	return NULL;
}

/* | function_name [il_operand_list] */
/* NOTE: The parameters 'called_function_declaration' and 'extensible_param_count' are used to pass data between the stage 3 and stage 4. */
// SYM_REF2(il_function_call_c, function_name, il_operand_list, symbol_c *called_function_declaration; int extensible_param_count;)
void *print_datatypes_error_c::visit(il_function_call_c *symbol) {
	/* The first parameter of a non formal function call in IL will be the 'current value' (i.e. the prev_il_instruction)
	 * In order to be able to handle this without coding special cases, we will simply prepend that symbol
	 * to the il_operand_list, and remove it after calling handle_function_call().
	 *
	 * However, if no further paramters are given, then il_operand_list will be NULL, and we will
	 * need to create a new object to hold the pointer to prev_il_instruction.
	 * This change will also be undone later in print_datatypes_error_c.
	 */
	if (NULL == symbol->il_operand_list)  symbol->il_operand_list = new il_operand_list_c;
	if (NULL == symbol->il_operand_list)  ERROR;

	((list_c *)symbol->il_operand_list)->insert_element(fake_prev_il_instruction, 0);

	generic_function_call_t fcall_param = {
		/* fcall_param.function_name               = */ symbol->function_name,
		/* fcall_param.nonformal_operand_list      = */ symbol->il_operand_list,
		/* fcall_param.formal_operand_list         = */ NULL,
		/* enum {POU_FB, POU_function} POU_type    = */ generic_function_call_t::POU_function,
		/* fcall_param.candidate_functions         = */ symbol->candidate_functions,
		/* fcall_param.called_function_declaration = */ symbol->called_function_declaration,
		/* fcall_param.extensible_param_count      = */ symbol->extensible_param_count
	};

/* TODO: check what error message (if any) the compiler will give out if this function invocation
 * is not preceded by a LD operator (or another equivalent operator or list of operators).
 */
	handle_function_invocation(symbol, fcall_param);
	
	/* We now undo those changes to the abstract syntax tree made above! */
	((list_c *)symbol->il_operand_list)->remove_element(0);
	if (((list_c *)symbol->il_operand_list)->n == 0) {
		/* if the list becomes empty, then that means that it did not exist before we made these changes, so we delete it! */
		delete 	symbol->il_operand_list;
		symbol->il_operand_list = NULL;
	}

	return NULL;
}


/* | il_expr_operator '(' [il_operand] eol_list [simple_instr_list] ')' */
// SYM_REF3(il_expression_c, il_expr_operator, il_operand, simple_instr_list);
void *print_datatypes_error_c::visit(il_expression_c *symbol) {
  /* first give the parenthesised IL list a chance to print errors */
  il_instruction_c *save_fake_prev_il_instruction = fake_prev_il_instruction;
  symbol->simple_instr_list->accept(*this);
  fake_prev_il_instruction = save_fake_prev_il_instruction;

  /* Now handle the operation (il_expr_operator) that will use the result coming from the parenthesised IL list (i.e. simple_instr_list) */
  il_operand = symbol->simple_instr_list; /* This is not a bug! The parenthesised expression will be used as the operator! */
  symbol->il_expr_operator->accept(*this);

return NULL;
}


/*   il_call_operator prev_declared_fb_name
 * | il_call_operator prev_declared_fb_name '(' ')'
 * | il_call_operator prev_declared_fb_name '(' eol_list ')'
 * | il_call_operator prev_declared_fb_name '(' il_operand_list ')'
 * | il_call_operator prev_declared_fb_name '(' eol_list il_param_list ')'
 */
/* NOTE: The parameter 'called_fb_declaration'is used to pass data between stage 3 and stage4 (although currently it is not used in stage 4 */
// SYM_REF4(il_fb_call_c, il_call_operator, fb_name, il_operand_list, il_param_list, symbol_c *called_fb_declaration)
void *print_datatypes_error_c::visit(il_fb_call_c *symbol) {
	int extensible_param_count;                      /* unused vairable! Needed for compilation only! */
	std::vector <symbol_c *> candidate_functions;    /* unused vairable! Needed for compilation only! */
	generic_function_call_t fcall_param = {
		/* fcall_param.function_name               = */ symbol->fb_name,
		/* fcall_param.nonformal_operand_list      = */ symbol->il_operand_list,
		/* fcall_param.formal_operand_list         = */ symbol->il_param_list,
		/* enum {POU_FB, POU_function} POU_type    = */ generic_function_call_t::POU_FB,
		/* fcall_param.candidate_functions         = */ candidate_functions,             /* will not be used, but must provide a reference to be able to compile */
		/* fcall_param.called_function_declaration = */ symbol->called_fb_declaration,
		/* fcall_param.extensible_param_count      = */ extensible_param_count           /* will not be used, but must provide a reference to be able to compile */
	};
  
	handle_function_invocation(symbol, fcall_param);
	/* check the semantics of the CALC, CALCN operators! */
	symbol->il_call_operator->accept(*this);
	return NULL;
}

/* | function_name '(' eol_list [il_param_list] ')' */
/* NOTE: The parameter 'called_function_declaration' is used to pass data between the stage 3 and stage 4. */
// SYM_REF2(il_formal_funct_call_c, function_name, il_param_list, symbol_c *called_function_declaration; int extensible_param_count;)
void *print_datatypes_error_c::visit(il_formal_funct_call_c *symbol) {
	generic_function_call_t fcall_param = {
		/* fcall_param.function_name               = */ symbol->function_name,
		/* fcall_param.nonformal_operand_list      = */ NULL,
		/* fcall_param.formal_operand_list         = */ symbol->il_param_list,
		/* enum {POU_FB, POU_function} POU_type    = */ generic_function_call_t::POU_function,
		/* fcall_param.candidate_functions         = */ symbol->candidate_functions,
		/* fcall_param.called_function_declaration = */ symbol->called_function_declaration,
		/* fcall_param.extensible_param_count      = */ symbol->extensible_param_count
	};
  
	handle_function_invocation(symbol, fcall_param);
	return NULL;
}


//     void *visit(il_operand_list_c *symbol);
//     void *visit(simple_instr_list_c *symbol);

// SYM_REF1(il_simple_instruction_c, il_simple_instruction, symbol_c *prev_il_instruction;)
void *print_datatypes_error_c::visit(il_simple_instruction_c *symbol)	{
  if (symbol->prev_il_instruction.size() > 1) ERROR; /* There should be no labeled insructions inside an IL expression! */
    
  il_instruction_c tmp_prev_il_instruction(NULL, NULL);
  /* the print error algorithm will need access to the intersected candidate_datatype lists of all prev_il_instructions, as well as the 
   * list of the prev_il_instructions.
   * Instead of creating two 'global' (within the class) variables, we create a single il_instruction_c variable (fake_prev_il_instruction),
   * and shove that data into this single variable.
   */
  if (symbol->prev_il_instruction.size() > 0)
    tmp_prev_il_instruction.candidate_datatypes = symbol->prev_il_instruction[0]->candidate_datatypes;
  tmp_prev_il_instruction.prev_il_instruction = symbol->prev_il_instruction;
  
   /* copy the candidate_datatypes list */
  fake_prev_il_instruction = &tmp_prev_il_instruction;
  symbol->il_simple_instruction->accept(*this);
  fake_prev_il_instruction = NULL;
  return NULL;
  
  
//   if (symbol->prev_il_instruction.size() == 0)  prev_il_instruction = NULL;
//   else                                          prev_il_instruction = symbol->prev_il_instruction[0];

//   symbol->il_simple_instruction->accept(*this);
//   prev_il_instruction = NULL;
//   return NULL;
}


//     void *visit(il_param_list_c *symbol);
//     void *visit(il_param_assignment_c *symbol);
//     void *visit(il_param_out_assignment_c *symbol);

/*******************/
/* B 2.2 Operators */
/*******************/
void *print_datatypes_error_c::print_binary_operator_errors(const char *il_operator, symbol_c *symbol, bool deprecated_operation) {
	if ((symbol->candidate_datatypes.size() == 0) && (il_operand->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for '%s' operator.", il_operator);
	} else if (NULL == symbol->datatype) {
		STAGE3_WARNING(symbol, symbol, "Result of '%s' operation is never used.", il_operator);
	} else if (deprecated_operation)
		STAGE3_WARNING(symbol, symbol, "Deprecated operation for '%s' operator.", il_operator);
	return NULL;
}


void *print_datatypes_error_c::visit(LD_operator_c *symbol) {
	return NULL;
}

void *print_datatypes_error_c::visit(LDN_operator_c *symbol) {
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(il_operand->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'LDN' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit(ST_operator_c *symbol) {
	/* MANU:
	 * if prev_instruction is NULL we can print a message error or warning error like:
	 * we can't use a ST like first instruction.
	 * What do you think?
	 */
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(il_operand->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'ST' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit(STN_operator_c *symbol) {
	/* MANU:
	 * if prev_instruction is NULL we can print a message error or warning error like:
	 * we can't use a ST like first instruction.
	 * What do you think?
	 */
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(il_operand->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'STN' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit(NOT_operator_c *symbol) {
	/* NOTE: the standard allows syntax in which the NOT operator is followed by an optional <il_operand>
	 *              NOT [<il_operand>]
	 *       However, it does not define the semantic of the NOT operation when the <il_operand> is specified.
	 *       We therefore consider it an error if an il_operand is specified!
	 */
	if (il_operand != NULL)
		STAGE3_ERROR(0, symbol, symbol, "'NOT' operator may not have an operand.");
	if (symbol->candidate_datatypes.size() == 0)
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'NOT' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit(S_operator_c *symbol) {
  /* TODO: what if this is a FB call ?? */
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(il_operand->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'S' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit(R_operator_c *symbol) {
  /* TODO: what if this is a FB call ?? */
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(il_operand->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for 'R' operator.");
	return NULL;
}

void *print_datatypes_error_c::visit( S1_operator_c *symbol) {return handle_implicit_il_fb_invocation( "S1", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( R1_operator_c *symbol) {return handle_implicit_il_fb_invocation( "R1", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit(CLK_operator_c *symbol) {return handle_implicit_il_fb_invocation("CLK", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( CU_operator_c *symbol) {return handle_implicit_il_fb_invocation( "CU", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( CD_operator_c *symbol) {return handle_implicit_il_fb_invocation( "CD", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( PV_operator_c *symbol) {return handle_implicit_il_fb_invocation( "PV", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( IN_operator_c *symbol) {return handle_implicit_il_fb_invocation( "IN", symbol, symbol->called_fb_declaration);}
void *print_datatypes_error_c::visit( PT_operator_c *symbol) {return handle_implicit_il_fb_invocation( "PT", symbol, symbol->called_fb_declaration);}

void *print_datatypes_error_c::visit( AND_operator_c *symbol) {return print_binary_operator_errors("AND" , symbol);}
void *print_datatypes_error_c::visit(  OR_operator_c *symbol) {return print_binary_operator_errors( "OR" , symbol);}
void *print_datatypes_error_c::visit( XOR_operator_c *symbol) {return print_binary_operator_errors("XOR" , symbol);}
void *print_datatypes_error_c::visit(ANDN_operator_c *symbol) {return print_binary_operator_errors("ANDN", symbol);}
void *print_datatypes_error_c::visit( ORN_operator_c *symbol) {return print_binary_operator_errors( "ORN", symbol);}
void *print_datatypes_error_c::visit(XORN_operator_c *symbol) {return print_binary_operator_errors("XORN", symbol);}
void *print_datatypes_error_c::visit( ADD_operator_c *symbol) {return print_binary_operator_errors("ADD" , symbol, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit( SUB_operator_c *symbol) {return print_binary_operator_errors("SUB" , symbol, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit( MUL_operator_c *symbol) {return print_binary_operator_errors("MUL" , symbol, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit( DIV_operator_c *symbol) {return print_binary_operator_errors("DIV" , symbol, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit( MOD_operator_c *symbol) {return print_binary_operator_errors("MOD" , symbol);}

void *print_datatypes_error_c::visit(  GT_operator_c *symbol) {return print_binary_operator_errors( "GT" , symbol);}
void *print_datatypes_error_c::visit(  GE_operator_c *symbol) {return print_binary_operator_errors( "GE" , symbol);}
void *print_datatypes_error_c::visit(  EQ_operator_c *symbol) {return print_binary_operator_errors( "EQ" , symbol);}
void *print_datatypes_error_c::visit(  LT_operator_c *symbol) {return print_binary_operator_errors( "LT" , symbol);}
void *print_datatypes_error_c::visit(  LE_operator_c *symbol) {return print_binary_operator_errors( "LE" , symbol);}
void *print_datatypes_error_c::visit(  NE_operator_c *symbol) {return print_binary_operator_errors( "NE" , symbol);}

  
void *print_datatypes_error_c::visit(CAL_operator_c *symbol) {
	return NULL;
}


void *print_datatypes_error_c::handle_conditional_flow_control_IL_instruction(symbol_c *symbol, const char *oper) {
	if (NULL == symbol->datatype)
		STAGE3_ERROR(0, symbol, symbol, "%s operator must be preceded by an IL instruction producing a BOOL value.", oper);
	return NULL;
}

void *print_datatypes_error_c::visit(CALC_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "CALC");
}

void *print_datatypes_error_c::visit(CALCN_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "CALCN");
}

void *print_datatypes_error_c::visit(RET_operator_c *symbol) {
	return NULL;
}

void *print_datatypes_error_c::visit(RETC_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "RETC");
}

void *print_datatypes_error_c::visit(RETCN_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "RETCN");
}

void *print_datatypes_error_c::visit(JMP_operator_c *symbol) {
	return NULL;
}

void *print_datatypes_error_c::visit(JMPC_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "JMPC");
}

void *print_datatypes_error_c::visit(JMPCN_operator_c *symbol) {
	return handle_conditional_flow_control_IL_instruction(symbol, "JMPCN");
}

/* Symbol class handled together with function call checks */
// void *visit(il_assign_operator_c *symbol, variable_name);
/* Symbol class handled together with function call checks */
// void *visit(il_assign_operator_c *symbol, option, variable_name);

/***************************************/
/* B.3 - Language ST (Structured Text) */
/***************************************/
/***********************/
/* B 3.1 - Expressions */
/***********************/

void *print_datatypes_error_c::print_binary_expression_errors(const char *operation, symbol_c *symbol, symbol_c *l_expr, symbol_c *r_expr, bool deprecated_operation) {
	l_expr->accept(*this);
	r_expr->accept(*this);
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(l_expr->candidate_datatypes.size() > 0)	&&
		(r_expr->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for '%s' expression.", operation);
        if (deprecated_operation)
                STAGE3_WARNING(symbol, symbol, "Deprecated operation for '%s' expression.", operation);
	return NULL;
}


void *print_datatypes_error_c::visit(    or_expression_c *symbol) {return print_binary_expression_errors( "OR", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(   xor_expression_c *symbol) {return print_binary_expression_errors("XOR", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(   and_expression_c *symbol) {return print_binary_expression_errors("AND", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(   equ_expression_c *symbol) {return print_binary_expression_errors( "=" , symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(notequ_expression_c *symbol) {return print_binary_expression_errors( "<>", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(    lt_expression_c *symbol) {return print_binary_expression_errors( "<" , symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(    gt_expression_c *symbol) {return print_binary_expression_errors( ">" , symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(    le_expression_c *symbol) {return print_binary_expression_errors( "<=", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(    ge_expression_c *symbol) {return print_binary_expression_errors( ">=", symbol, symbol->l_exp, symbol->r_exp);}
void *print_datatypes_error_c::visit(   add_expression_c *symbol) {return print_binary_expression_errors( "+" , symbol, symbol->l_exp, symbol->r_exp, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit(   sub_expression_c *symbol) {return print_binary_expression_errors( "-" , symbol, symbol->l_exp, symbol->r_exp, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit(   mul_expression_c *symbol) {return print_binary_expression_errors( "*" , symbol, symbol->l_exp, symbol->r_exp, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit(   div_expression_c *symbol) {return print_binary_expression_errors( "/" , symbol, symbol->l_exp, symbol->r_exp, symbol->deprecated_operation);}
void *print_datatypes_error_c::visit(   mod_expression_c *symbol) {return print_binary_expression_errors("MOD", symbol, symbol->l_exp, symbol->r_exp);}


void *print_datatypes_error_c::visit(power_expression_c *symbol) {
	symbol->l_exp->accept(*this);
	symbol->r_exp->accept(*this);
	if ((symbol->candidate_datatypes.size() == 0) 		&&
		(symbol->l_exp->candidate_datatypes.size() > 0)	&&
		(symbol->r_exp->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Data type mismatch for '**' expression.");
	return NULL;
}


void *print_datatypes_error_c::visit(neg_expression_c *symbol) {
	symbol->exp->accept(*this);
	if ((symbol->candidate_datatypes.size() == 0)      &&
		(symbol->exp->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'NEG' expression.");
	return NULL;
}


void *print_datatypes_error_c::visit(not_expression_c *symbol) {
	symbol->exp->accept(*this);
	if ((symbol->candidate_datatypes.size() == 0)      &&
		(symbol->exp->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'NOT' expression.");
	return NULL;
}

/* NOTE: The parameter 'called_function_declaration', 'extensible_param_count' and 'candidate_functions' are used to pass data between the stage 3 and stage 4. */
/*    formal_param_list -> may be NULL ! */
/* nonformal_param_list -> may be NULL ! */
// SYM_REF3(function_invocation_c, function_name, formal_param_list, nonformal_param_list, symbol_c *called_function_declaration; int extensible_param_count; std::vector <symbol_c *> candidate_functions;)
void *print_datatypes_error_c::visit(function_invocation_c *symbol) {
	generic_function_call_t fcall_param = {
		/* fcall_param.function_name               = */ symbol->function_name,
		/* fcall_param.nonformal_operand_list      = */ symbol->nonformal_param_list,
		/* fcall_param.formal_operand_list         = */ symbol->formal_param_list,
		/* enum {POU_FB, POU_function} POU_type    = */ generic_function_call_t::POU_function,
		/* fcall_param.candidate_functions         = */ symbol->candidate_functions,
		/* fcall_param.called_function_declaration = */ symbol->called_function_declaration,
		/* fcall_param.extensible_param_count      = */ symbol->extensible_param_count
	};

	handle_function_invocation(symbol, fcall_param);
	return NULL;
}



/********************/
/* B 3.2 Statements */
/********************/

/*********************************/
/* B 3.2.1 Assignment Statements */
/*********************************/
void *print_datatypes_error_c::visit(assignment_statement_c *symbol) {
	symbol->l_exp->accept(*this);
	symbol->r_exp->accept(*this);
	if ((NULL == symbol->l_exp->datatype) &&
	    (NULL == symbol->r_exp->datatype) &&
		(symbol->l_exp->candidate_datatypes.size() > 0)	&&
		(symbol->r_exp->candidate_datatypes.size() > 0))
		STAGE3_ERROR(0, symbol, symbol, "Invalid data types for ':=' operation.");
	return NULL;
}


/*****************************************/
/* B 3.2.2 Subprogram Control Statements */
/*****************************************/
/* fb_name '(' [param_assignment_list] ')' */
/*    formal_param_list -> may be NULL ! */
/* nonformal_param_list -> may be NULL ! */
/* NOTE: The parameter 'called_fb_declaration'is used to pass data between stage 3 and stage4 (although currently it is not used in stage 4 */
// SYM_REF3(fb_invocation_c, fb_name, formal_param_list, nonformal_param_list, symbol_c *called_fb_declaration;)
void *print_datatypes_error_c::visit(fb_invocation_c *symbol) {
	int extensible_param_count;                      /* unused vairable! Needed for compilation only! */
	std::vector <symbol_c *> candidate_functions;    /* unused vairable! Needed for compilation only! */
	generic_function_call_t fcall_param = {
		/* fcall_param.function_name               = */ symbol->fb_name,
		/* fcall_param.nonformal_operand_list      = */ symbol->nonformal_param_list,
		/* fcall_param.formal_operand_list         = */ symbol->formal_param_list,
		/* enum {POU_FB, POU_function} POU_type    = */ generic_function_call_t::POU_FB,
		/* fcall_param.candidate_functions         = */ candidate_functions,             /* will not be used, but must provide a reference to be able to compile */
		/* fcall_param.called_function_declaration = */ symbol->called_fb_declaration,
		/* fcall_param.extensible_param_count      = */ extensible_param_count           /* will not be used, but must provide a reference to be able to compile */
	};
	
	handle_function_invocation(symbol, fcall_param);
	return NULL;
}


/********************************/
/* B 3.2.3 Selection Statements */
/********************************/

void *print_datatypes_error_c::visit(if_statement_c *symbol) {
	symbol->expression->accept(*this);
	if ((NULL == symbol->expression->datatype) &&
		(symbol->expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'IF' condition.");
	}
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);
	if (NULL != symbol->elseif_statement_list)
		symbol->elseif_statement_list->accept(*this);
	if (NULL != symbol->else_statement_list)
		symbol->else_statement_list->accept(*this);
	return NULL;
}

void *print_datatypes_error_c::visit(elseif_statement_c *symbol) {
	symbol->expression->accept(*this);
	if ((NULL == symbol->expression->datatype) &&
		(symbol->expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'ELSIF' condition.");
	}
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);
	return NULL;
}


void *print_datatypes_error_c::visit(case_statement_c *symbol) {
	symbol->expression->accept(*this);
	if ((NULL == symbol->expression->datatype) &&
		(symbol->expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "'CASE' quantity not an integer or enumerated.");
	}
	symbol->case_element_list->accept(*this);
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);
	return NULL;
}

/********************************/
/* B 3.2.4 Iteration Statements */
/********************************/

void *print_datatypes_error_c::visit(for_statement_c *symbol) {
	symbol->control_variable->accept(*this);
	symbol->beg_expression->accept(*this);
	symbol->end_expression->accept(*this);
	/* Control variable */
	if ((NULL == symbol->control_variable->datatype) &&
		(symbol->control_variable->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'FOR' control variable.");
	}
	/* BEG expression */
	if ((NULL == symbol->beg_expression->datatype) &&
		(symbol->beg_expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'FOR' begin expression.");
	}
	/* END expression */
	if ((NULL == symbol->end_expression->datatype) &&
		(symbol->end_expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'FOR' end expression.");
	}
	/* BY expression */
	if ((NULL != symbol->by_expression) &&
		(NULL == symbol->by_expression->datatype) &&
		(symbol->end_expression->candidate_datatypes.size() > 0)) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'FOR' by expression.");
	}
	/* DO statement */
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);

	return NULL;
}

void *print_datatypes_error_c::visit(while_statement_c *symbol) {
	symbol->expression->accept(*this);
	if (symbol->candidate_datatypes.size() != 1) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'WHILE' condition.");
		return NULL;
	}
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);
	return NULL;
}

void *print_datatypes_error_c::visit(repeat_statement_c *symbol) {
	if (symbol->candidate_datatypes.size() != 1) {
		STAGE3_ERROR(0, symbol, symbol, "Invalid data type for 'REPEAT' condition.");
		return NULL;
	}
	if (NULL != symbol->statement_list)
		symbol->statement_list->accept(*this);
	symbol->expression->accept(*this);
	return NULL;
}






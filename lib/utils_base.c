/*****************************************************************************
 *
 * utils_base.c
 *
 * Library of useful functions for plugins
 * These functions are tested with libtap. See tests/ directory
 *
 * Copyright (c) 2006 Nagios Plugin Development Team
 * License: GPL
 *
 * $Revision$
 * $Date$
 ****************************************************************************/

#include "common.h"
#include "utils_base.h"

void
die (int result, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vprintf (fmt, ap);
	va_end (ap);
	exit (result);
}

void set_range_start (range *this, double value) {
	this->start = value;
	this->start_infinity = FALSE;
}

void set_range_end (range *this, double value) {
	this->end = value;
	this->end_infinity = FALSE;
}

range
*parse_range_string (char *str) {
	range *temp_range;
	double start;
	double end;
	char *end_str;

	temp_range = (range *) malloc(sizeof(range));

	/* Set defaults */
	temp_range->start = 0;
	temp_range->start_infinity = FALSE;
	temp_range->end = 0;
	temp_range->end_infinity = TRUE;
	temp_range->alert_on = OUTSIDE;

	if (str[0] == '@') {
		temp_range->alert_on = INSIDE;
		str++;
	}

	end_str = index(str, ':');
	if (end_str != NULL) {
		if (str[0] == '~') {
			temp_range->start_infinity = TRUE;
		} else {
			start = strtod(str, NULL);	/* Will stop at the ':' */
			set_range_start(temp_range, start);
		}
		end_str++;		/* Move past the ':' */
	} else {
		end_str = str;
	}
	end = strtod(end_str, NULL);
	if (strcmp(end_str, "") != 0) {
		set_range_end(temp_range, end);
	}

	if (temp_range->start_infinity == TRUE || 
		temp_range->end_infinity == TRUE ||
		temp_range->start <= temp_range->end) {
		return temp_range;
	}
	free(temp_range);
	return NULL;
}

/* returns 0 if okay, otherwise 1 */
int
_set_thresholds(thresholds **my_thresholds, char *warn_string, char *critical_string)
{
	thresholds *temp_thresholds = NULL;

	temp_thresholds = malloc(sizeof(temp_thresholds));

	temp_thresholds->warning = NULL;
	temp_thresholds->critical = NULL;

	if (warn_string != NULL) {
		if ((temp_thresholds->warning = parse_range_string(warn_string)) == NULL) {
			return 1;
		}
	}
	if (critical_string != NULL) {
		if ((temp_thresholds->critical = parse_range_string(critical_string)) == NULL) {
			return 1;
		}
	}

	if (*my_thresholds > 0) {	/* Not sure why, but sometimes could be -1 */
		/* printf("Freeing here: %d\n", *my_thresholds); */
		free(*my_thresholds);
	}
	*my_thresholds = temp_thresholds;

	return 0;
}

void
set_thresholds(thresholds **my_thresholds, char *warn_string, char *critical_string)
{
	if (_set_thresholds(my_thresholds, warn_string, critical_string) == 0) {
		return;
	} else {
		die(STATE_UNKNOWN, _("Range format incorrect"));
	}
}

void print_thresholds(const char *threshold_name, thresholds *my_threshold) {
	printf("%s - ", threshold_name);
	if (! my_threshold) {
		printf("Threshold not set");
	} else {
		if (my_threshold->warning) {
			printf("Warning: start=%g end=%g; ", my_threshold->warning->start, my_threshold->warning->end);
		} else {
			printf("Warning not set; ");
		}
		if (my_threshold->critical) {
			printf("Critical: start=%g end=%g", my_threshold->critical->start, my_threshold->critical->end);
		} else {
			printf("Critical not set");
		}
	}
	printf("\n");
}

/* Returns TRUE if alert should be raised based on the range */
int
check_range(double value, range *my_range)
{
	int false = FALSE;
	int true = TRUE;
	
	if (my_range->alert_on == INSIDE) {
		false = TRUE;
		true = FALSE;
	}

	if (my_range->end_infinity == FALSE && my_range->start_infinity == FALSE) {
		if ((my_range->start <= value) && (value <= my_range->end)) {
			return false;
		} else {
			return true;
		}
	} else if (my_range->start_infinity == FALSE && my_range->end_infinity == TRUE) {
		if (my_range->start <= value) {
			return false;
		} else {
			return true;
		}
	} else if (my_range->start_infinity == TRUE && my_range->end_infinity == FALSE) {
		if (value <= my_range->end) {
			return false;
		} else {
			return true;
		}
	} else {
		return false;
	}
}

/* Returns status */
int
get_status(double value, thresholds *my_thresholds)
{
	if (my_thresholds->critical != NULL) {
		if (check_range(value, my_thresholds->critical) == TRUE) {
			return STATE_CRITICAL;
		}
	}
	if (my_thresholds->warning != NULL) {
		if (check_range(value, my_thresholds->warning) == TRUE) {
			return STATE_WARNING;
		}
	}
	return STATE_OK;
}

char *np_escaped_string (const char *string) {
	char *data;
	int i, j=0;
	data = strdup(string);
	for (i=0; data[i]; i++) {
		if (data[i] == '\\') {
			switch(data[++i]) {
				case 'n':
					data[j++] = '\n';
					break;
				case 'r':
					data[j++] = '\r';
					break;
				case 't':
					data[j++] = '\t';
					break;
				case '\\':
					data[j++] = '\\';
					break;
				default:
					data[j++] = data[i];
			}
		} else {
			data[j++] = data[i];
		}
	}
	data[j] = '\0';
	return data;
}
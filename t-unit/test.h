#ifndef PACHI_T_UNIT_TEST_H
#define PACHI_T_UNIT_TEST_H

/* run single unit test */
int unit_test_cmd(struct board *b, char *line);

/* run all unit tests in file */
int unit_test(char *filename);

/* number of mc simulations for score estimator. */
extern int estimator_simulations;

#endif

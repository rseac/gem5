#ifndef _BLACKSCHOLES_H_
#define _BLACKSCHOLES_H_

#include <stdint.h>
#include <stddef.h>

typedef float fptype;

typedef struct OptionData_ {
    fptype s;          // spot price
    fptype strike;     // strike price
    fptype r;          // risk-free rate
    fptype divq;       // dividend rate
    fptype v;          // volatility
    fptype t;          // time to maturity
    char OptionType;   // 'P' or 'C'
    fptype divs;       // dividend values
    fptype DGrefval;   // derivative price
} OptionData;

void blackscholes_vector(int numOptions, fptype *sptprice, fptype *strike,
                         fptype *rate, fptype *volatility, fptype *time,
                         int *otype, fptype *prices);

int blackscholes_verify(int numOptions, fptype *sptprice, fptype *strike,
                        fptype *rate, fptype *volatility, fptype *time,
                        int *otype, fptype *prices);

#endif

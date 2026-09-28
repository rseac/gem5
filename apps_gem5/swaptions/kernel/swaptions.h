#ifndef SWAPTIONS_H
#define SWAPTIONS_H

#include <stdint.h>
#include <stddef.h>

#define NUM_SWAPTIONS 32
#define NUM_FACTORS 3
#define NUM_TIME_STEPS 10

typedef struct {
    double dSimSwaptionPrice;
    double dSimSwaptionPriceError;
    int iN;
    int iFactors;
    double dYears;
    double dStrike;
    double dCompounding;
    double dMaturity;
    double dTenor;
    double dPaymentInterval;
} parm;

void swaptions_init(int num_swaptions, double *swaption_yields, double *swaption_vols);
void swaptions_vector(int num_swaptions, double *swaption_yields, double *swaption_vols, double *sim_prices);
int swaptions_verify(int num_swaptions, double *sim_prices);

#endif

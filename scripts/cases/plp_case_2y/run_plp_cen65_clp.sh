#!/bin/bash
#
#
#
# Binario de PLP
#

PLPBIN=/opt/plp_cen65/plp

#
# Interfaz de usuario
#
export PLP_INTERFAZ_MODE=no

#
# Convenios de Riego
#
export PLP_CONVLAJA_MODE=2
export PLP_CONVMAULE_MODE=2
export PLP_RESTRALCO_MODE=no

#
# Correccion potencia
#
export PLP_RENDBDRS_MODE=si

#
# Escalamiento
#
export PLP_SCALE_MODE=si
export PLP_SCALEOBJ_MODE=1e3
export PLP_SCALEPHI_MODE=1e6
export PLP_ANGZERO_MODE=si
#export PLP_SCALEVDI_MODE=no

#
# Parametros de cortes de optimalidad y factibilidad
#
export PLP_OPTIEPS_VALUE=1e-12
export PLP_OPTIMLD_VALUE=1e10
export PLP_FACTEPS_VALUE=1e-10
export PLP_FACTMLD_VALUE=1000
#
# Habiliar cortes de factibilidad
#
export PLP_FACTMXC_VALUE=500
export PLP_FACTDBL_VALUE=1
export PLP_FACT_MODE=3
# Modo antiguo de cortes
#export PLP_FONEFEASRAY_MODE=si

#
# Tolerancias
#
export PLP_EPOPT_VALUE=1e-9
export PLP_EPRHS_VALUE=1e-9

#
# Afluentes ficticios
#
export PLP_AFLUFICT_MODE=no

#Modos de uso de cortes de optimalidad
#
#export PLP_ONEPHI_MODE=no
#export PLP_SEPARACF_MODE=si
#export PLP_ZSPFBEST_MODE=no

#
# OMP y Paralel mode
#
export PLP_PARAL_MODE=si
# export OMP_SCHEDULE=dynamic,2
# export OMP_STACKSIZE=4048M
# export OMP_NUM_THREADS=6

# limpieza de archivos
rm -fr *.lp
rm -fr *.log
rm -fr *_sim.csv
#
#  Ejecucion binario
#
exec time $PLPBIN

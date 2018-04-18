#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/collections/list.h>
#include "libs/socketCommons.h"
#include "libs/serialize.h"

/*MACROS*/
#define PATH_FILE_NAME "planificador.config"

#ifndef PLANIFICADOR_SRC_PLANIFICADOR_H_
#define PLANIFICADOR_SRC_PLANIFICADOR_H_

t_log *console_log;

enum AlgortimoPlanificacion {
	SJF_CD = 1, SJF_SD = 2, HRRN = 3
};

struct {
	char* IP_COORDINADOR;
	int PUERTO_COORDINADOR;
	enum AlgortimoPlanificacion ALGORITMO_PLANIFICACION;
	int ESTIMACION_INICIAL;
	int PUERTO_ESCUCHA_CONEXIONES;
	char** CLAVES_INICIALMENTE_BLOQUEADAS;
} planificador_setup;


/*FUNCIONES*/
int readConfig(char* configFile);

// Funciones de la aplicacion del algoritmo
void applyPlaningAlgorithm();
void getNextESI();
void moveOutCurrentESI();
void continueExecutingESI();

//Funciones para los recursos
bool isResourceAvailable();
void lockResource();
void unlockResource();

//Funciones para la administracion de los ESI
void lockESI();
void unlockESI();
void finishESI();

//Comunicacion con el coordinador
void sendLockResourceOperationResult(bool p_result);
void sendUnlockResourceOperationResult(bool p_result);

#endif /* PLANIFICADOR_SRC_PLANIFICADOR_H_ */

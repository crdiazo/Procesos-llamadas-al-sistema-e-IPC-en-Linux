/*
 * ============================================================================
 *  csv.h
 *  Módulo de búsqueda secuencial sobre el archivo CSV. Lo usan los dos
 *  servidores (FIFO y SHM), de modo que la lógica de negocio es idéntica y
 *  sólo cambia el mecanismo IPC.
 *
 *  IMPORTANTE: el dataset NUNCA se carga completo en memoria. En cada
 *  consulta se abre el archivo y se lee línea a línea con getline(); por
 *  cada registro que cumple el criterio se invoca una función de retorno
 *  (callback) que lo envía inmediatamente al cliente.
 * ============================================================================
 */
#ifndef CSV_H
#define CSV_H

#include <stddef.h>

#include "protocolo.h"

/*
 * Tipo de la función que se invoca por cada registro coincidente.
 * Debe devolver 0 para continuar o distinto de 0 para abortar la búsqueda
 * (por ejemplo, porque el cliente desapareció o llegó una señal).
 */
typedef int (*funcion_coincidencia_t)(const registro_t *registro, void *contexto);

/*
 * Interpreta una línea "id,nombre,categoria,precio,stock,fecha".
 * Modifica 'linea' (sustituye las comas por '\0').
 * Devuelve 0 si la línea es válida o -1 si su formato es incorrecto.
 */
int parsear_linea_csv(char *linea, registro_t *registro);

/* Devuelve 1 si 'registro' cumple el criterio de 'solicitud', 0 si no. */
int registro_cumple(const registro_t *registro, const solicitud_t *solicitud);

/*
 * Valida los parámetros de una solicitud de búsqueda.
 * Devuelve 0 si es válida; -1 y un mensaje descriptivo si no lo es.
 */
int validar_solicitud(const solicitud_t *solicitud, char *mensaje, size_t tam_mensaje);

/*
 * Recorre secuencialmente 'ruta_csv' y llama a 'al_coincidir' por cada
 * registro que cumple el criterio, hasta 'solicitud->limite' registros
 * (0 = sin límite). Se sigue leyendo hasta el final para contar el total
 * de coincidencias. Rellena 'resumen'.
 * Devuelve:
 *    0  búsqueda completa.
 *   -1  error de E/S con el CSV ('mensaje_error' describe la causa).
 *   -2  abortada porque 'al_coincidir' devolvió distinto de 0.
 */
int buscar_en_csv(const char *ruta_csv, const solicitud_t *solicitud,
                  funcion_coincidencia_t al_coincidir, void *contexto,
                  resumen_t *resumen, char *mensaje_error, size_t tam_mensaje);

/* Nombre legible de un código de operación (para trazas del servidor). */
const char *describir_operacion(int32_t operacion);

/* Descripción legible de una solicitud completa (para trazas). */
void describir_solicitud(const solicitud_t *solicitud, char *destino, size_t tam);

#endif /* CSV_H */

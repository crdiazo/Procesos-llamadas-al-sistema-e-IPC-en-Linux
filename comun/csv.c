/*
 * ============================================================================
 *  csv.c
 *  Implementación de la búsqueda secuencial sobre el CSV.
 *
 *  Formato esperado (primera línea = cabecera, opcional):
 *      id,nombre,categoria,precio,stock,fecha
 *      1,Telefono Pro Negro,Electronica,1234.56,42,2023-05-17
 *  Restricciones: separador ',', sin comillas ni comas dentro de los campos,
 *  precio con punto decimal, fecha AAAA-MM-DD.
 * ============================================================================
 */
#define _POSIX_C_SOURCE 200809L

#include "csv.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "utilidades.h"

/* Número de columnas del CSV */
#define NUM_CAMPOS_CSV 6

/* parsear_linea_csv: divide la línea en campos y los convierte.
    Se hace a mano (y no con strtok) para detectar campos vacíos. */
int parsear_linea_csv(char *linea, registro_t *registro)
{
    char *campos[NUM_CAMPOS_CSV];
    size_t num_campos = 0;
    char *cursor;
    char *fin;
    long long id;
    double precio;
    long stock;

    /* Quitar el fin de línea  */
    linea[strcspn(linea, "\r\n")] = '\0';
    if (linea[0] == '\0') {
        return -1;
    }

    /* Separar campos sustituyendo cada coma por '\0' */
    campos[num_campos++] = linea;
    for (cursor = linea; *cursor != '\0'; cursor++) {
        if (*cursor == ',') {
            if (num_campos == NUM_CAMPOS_CSV) {
                return -1; /* demasiadas columnas */
            }
            *cursor = '\0';
            campos[num_campos++] = cursor + 1;
        }
    }
    if (num_campos != NUM_CAMPOS_CSV) {
        return -1; /* faltan columnas */
    }

    /* id: entero */
    errno = 0;
    id = strtoll(campos[0], &fin, 10);
    if (errno != 0 || fin == campos[0] || *fin != '\0') {
        return -1;
    }

    /* precio: real no negativo */
    errno = 0;
    precio = strtod(campos[3], &fin);
    if (errno != 0 || fin == campos[3] || *fin != '\0' || !(precio >= 0.0)) {
        return -1;
    }

    /* stock: entero en rango de int32 */
    errno = 0;
    stock = strtol(campos[4], &fin, 10);
    if (errno != 0 || fin == campos[4] || *fin != '\0' ||
        stock < 0 || stock > INT32_MAX) {
        return -1;
    }

    registro->id = (int64_t) id;
    registro->precio = precio;
    registro->stock = (int32_t) stock;
    copiar_cadena(registro->nombre, sizeof registro->nombre, campos[1]);
    copiar_cadena(registro->categoria, sizeof registro->categoria, campos[2]);
    copiar_cadena(registro->fecha, sizeof registro->fecha, campos[5]);
    return 0;
}

/* registro_cumple: aplica el criterio de búsqueda.*/
int registro_cumple(const registro_t *registro, const solicitud_t *solicitud)
{
    switch (solicitud->operacion) {
    case OP_BUSCAR_CATEGORIA:
        return strcasecmp(registro->categoria, solicitud->texto) == 0;
    case OP_BUSCAR_PRECIO:
        return registro->precio >= solicitud->precio_min &&
               registro->precio <= solicitud->precio_max;
    case OP_BUSCAR_NOMBRE:
        return contiene_sin_mayusculas(registro->nombre, solicitud->texto);
    default:
        return 0;
    }
}

/* validar_solicitud: el servidor nunca confía en los datos recibidos.*/
int validar_solicitud(const solicitud_t *solicitud, char *mensaje, size_t tam_mensaje)
{
    if (solicitud->limite < 0) {
        snprintf(mensaje, tam_mensaje, "Límite de resultados inválido (%d)",
                 (int) solicitud->limite);
        return -1;
    }
    switch (solicitud->operacion) {
    case OP_BUSCAR_CATEGORIA:
    case OP_BUSCAR_NOMBRE:
        if (solicitud->texto[0] == '\0') {
            snprintf(mensaje, tam_mensaje, "El texto de búsqueda está vacío");
            return -1;
        }
        return 0;
    case OP_BUSCAR_PRECIO:
        if (!isfinite(solicitud->precio_min) || solicitud->precio_min < 0.0 ||
            isnan(solicitud->precio_max) ||
            solicitud->precio_max < solicitud->precio_min) {
            snprintf(mensaje, tam_mensaje,
                     "Rango de precio inválido [%.2f, %.2f]",
                     solicitud->precio_min, solicitud->precio_max);
            return -1;
        }
        return 0;
    default:
        snprintf(mensaje, tam_mensaje, "Operación desconocida (%d)",
                 (int) solicitud->operacion);
        return -1;
    }
}

/* buscar_en_csv: recorrido secuencial del archivo en disco.
  Memoria usada: un único búfer de línea (reservado dinámicamente por getline y liberado con free al terminar), independiente del tamaño del CSV. */
int buscar_en_csv(const char *ruta_csv, const solicitud_t *solicitud,
                  funcion_coincidencia_t al_coincidir, void *contexto,
                  resumen_t *resumen, char *mensaje_error, size_t tam_mensaje)
{
    struct timespec inicio;
    FILE *archivo;
    char *linea = NULL;      /* búfer dinámico gestionado por getline */
    size_t capacidad = 0;
    int64_t numero_linea = 0;
    int resultado = 0;

    memset(resumen, 0, sizeof *resumen);
    if (clock_gettime(CLOCK_MONOTONIC, &inicio) == -1) {
        memset(&inicio, 0, sizeof inicio);
    }

    archivo = fopen(ruta_csv, "r");
    if (archivo == NULL) {
        snprintf(mensaje_error, tam_mensaje, "No se pudo abrir '%s': %s",
                 ruta_csv, strerror(errno));
        return -1;
    }

    errno = 0;
    while (getline(&linea, &capacidad, archivo) != -1) {
        registro_t registro;

        numero_linea++;
        /* Saltar la cabecera si existe */
        if (numero_linea == 1 && strncmp(linea, "id,", 3) == 0) {
            continue;
        }
        resumen->lineas_leidas++;

        if (parsear_linea_csv(linea, &registro) == -1) {
            resumen->lineas_invalidas++;
            continue;
        }
        if (!registro_cumple(&registro, solicitud)) {
            continue;
        }

        resumen->coincidencias++;
        /* Se envía sólo hasta el límite, pero se sigue contando */
        if (solicitud->limite == 0 || resumen->enviados < solicitud->limite) {
            if (al_coincidir(&registro, contexto) != 0) {
                resultado = -2;
                break;
            }
            resumen->enviados++;
        }
    }

    /* getline devuelve -1 tanto en EOF como en error: distinguir con ferror */
    if (resultado == 0 && ferror(archivo)) {
        snprintf(mensaje_error, tam_mensaje, "Error leyendo '%s': %s",
                 ruta_csv, strerror(errno));
        resultado = -1;
    }

    free(linea);
    if (fclose(archivo) == EOF && resultado == 0) {
        snprintf(mensaje_error, tam_mensaje, "Error cerrando '%s': %s",
                 ruta_csv, strerror(errno));
        resultado = -1;
    }

    resumen->milisegundos = milisegundos_transcurridos(&inicio);
    return resultado;
}

/*  describir_operacion: texto legible para las trazas.*/
const char *describir_operacion(int32_t operacion)
{
    switch (operacion) {
    case OP_BUSCAR_CATEGORIA: return "BUSCAR_CATEGORIA";
    case OP_BUSCAR_PRECIO:    return "BUSCAR_PRECIO";
    case OP_BUSCAR_NOMBRE:    return "BUSCAR_NOMBRE";
    case OP_DESCONECTAR:      return "DESCONECTAR";
    case OP_TERMINAR:         return "TERMINAR";
    default:                  return "DESCONOCIDA";
    }
}

/*  describir_solicitud: operación + parámetros, para las trazas del servidor. */
void describir_solicitud(const solicitud_t *solicitud, char *destino, size_t tam)
{
    switch (solicitud->operacion) {
    case OP_BUSCAR_CATEGORIA:
    case OP_BUSCAR_NOMBRE:
        snprintf(destino, tam, "%s texto=\"%s\" limite=%d",
                 describir_operacion(solicitud->operacion), solicitud->texto,
                 (int) solicitud->limite);
        break;
    case OP_BUSCAR_PRECIO:
        snprintf(destino, tam, "%s [%.2f, %.2f] limite=%d",
                 describir_operacion(solicitud->operacion),
                 solicitud->precio_min, solicitud->precio_max,
                 (int) solicitud->limite);
        break;
    default:
        snprintf(destino, tam, "%s", describir_operacion(solicitud->operacion));
        break;
    }
}

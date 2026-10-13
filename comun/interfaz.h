/*
 * ============================================================================
 *  interfaz.h
 *  Interfaz de línea de comandos (CLI) común a los dos clientes. La
 *  funcionalidad visible para el usuario es idéntica en la versión FIFO y en
 *  la de memoria compartida; sólo cambia el mecanismo IPC subyacente.
 * ============================================================================
 */
#ifndef INTERFAZ_H
#define INTERFAZ_H

#include <stddef.h>

#include "protocolo.h"

/* Categorías disponibles en datos.csv (sólo informativo para el usuario) */
#define CATEGORIAS_EJEMPLO \
    "Electronica, Hogar, Deportes, Juguetes, Libros, Ropa, Alimentos, " \
    "Herramientas, Belleza, Jardin"

/* Opciones del menú principal */
enum {
    OPCION_SALIR     = 0,
    OPCION_CATEGORIA = 1,
    OPCION_PRECIO    = 2,
    OPCION_NOMBRE    = 3,
    OPCION_LIMITE    = 4,
    OPCION_DETENER   = 5,
    OPCION_MAXIMA    = 5
};

/*
 * Lee una línea de la entrada estándar (sin '\n' y sin espacios en los
 * extremos). Devuelve 0 o -1 si hubo EOF o la lectura fue interrumpida por
 * una señal.
 */
int leer_linea(const char *mensaje, char *buffer, size_t tam);

/* Lee un entero en [minimo, maximo], repitiendo si es inválido. 0 / -1. */
int leer_entero(const char *mensaje, long minimo, long maximo, long *valor);

/* Lee un real finito >= minimo, repitiendo si es inválido. 0 / -1. */
int leer_real(const char *mensaje, double minimo, double *valor);

/* Muestra el menú principal. */
void mostrar_menu(const char *version, int limite);

/*
 * Rellena 'solicitud' según la opción elegida, pidiendo al usuario los
 * parámetros necesarios. Devuelve 0 o -1 si la entrada terminó.
 */
int construir_solicitud(int opcion, int limite, solicitud_t *solicitud);

/* Impresión de resultados en formato de tabla. */
void imprimir_encabezado_resultados(void);
void imprimir_registro(const registro_t *registro);

/* Imprime el resumen; si no hubo coincidencias, "No se encontraron resultados". */
void imprimir_resumen(const resumen_t *resumen);

#endif /* INTERFAZ_H */

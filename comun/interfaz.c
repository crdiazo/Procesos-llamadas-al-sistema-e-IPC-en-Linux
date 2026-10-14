/*
 * ============================================================================
 *  interfaz.c
 *  Implementación de la CLI común a los clientes.
 * ============================================================================
 */
#define _POSIX_C_SOURCE 200809L

#include "interfaz.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void recortar_espacios(char *texto)
{
    size_t longitud = strlen(texto);
    size_t inicio = 0;

    while (longitud > 0 && isspace((unsigned char) texto[longitud - 1])) {
        texto[--longitud] = '\0';
    }
    while (texto[inicio] != '\0' && isspace((unsigned char) texto[inicio])) {
        inicio++;
    }
    if (inicio > 0) {
        memmove(texto, texto + inicio, longitud - inicio + 1);
    }
}

// leer_linea: fgets devuelve NULL en EOF (Ctrl+D) o si una señal interrumpe
int leer_linea(const char *mensaje, char *buffer, size_t tam)
{
    size_t longitud;

    if (tam < 2) {
        return -1;
    }
    printf("%s", mensaje);
    fflush(stdout);

    if (fgets(buffer, (int) tam, stdin) == NULL) {
        clearerr(stdin);
        return -1;
    }
    longitud = strcspn(buffer, "\n");
    if (buffer[longitud] == '\n') {
        buffer[longitud] = '\0';
    } else if (longitud == tam - 1) {
        int caracter;
        while ((caracter = getchar()) != '\n' && caracter != EOF) {
        }
    }
    recortar_espacios(buffer);
    return 0;
}

// leer_entero: conversión estricta con strtol.
int leer_entero(const char *mensaje, long minimo, long maximo, long *valor)
{
    char buffer[64];

    for (;;) {
        char *fin;
        long numero;

        if (leer_linea(mensaje, buffer, sizeof buffer) == -1) {
            return -1;
        }
        errno = 0;
        numero = strtol(buffer, &fin, 10);
        if (buffer[0] != '\0' && *fin == '\0' && errno == 0 &&
            numero >= minimo && numero <= maximo) {
            *valor = numero;
            return 0;
        }
        printf("  Valor invalido: introduzca un entero entre %ld y %ld.\n",
               minimo, maximo);
    }
}

// leer_real: conversion estricta con strtod (rechaza nan/inf).
int leer_real(const char *mensaje, double minimo, double *valor)
{
    char buffer[64];

    for (;;) {
        char *fin;
        double numero;

        if (leer_linea(mensaje, buffer, sizeof buffer) == -1) {
            return -1;
        }
        errno = 0;
        numero = strtod(buffer, &fin);
        if (buffer[0] != '\0' && *fin == '\0' && errno == 0 &&
            isfinite(numero) && numero >= minimo) {
            *valor = numero;
            return 0;
        }
        printf("  Valor invalido: introduzca un numero >= %.2f (use '.' como "
               "separador decimal).\n", minimo);
    }
}

// leer_texto_busqueda: texto no vacío y sin comas (separador del CSV).
static int leer_texto_busqueda(const char *mensaje, char *buffer, size_t tam)
{
    for (;;) {
        if (leer_linea(mensaje, buffer, tam) == -1) {
            return -1;
        }
        if (buffer[0] != '\0' && strchr(buffer, ',') == NULL) {
            return 0;
        }
        printf("  El texto no puede estar vacio ni contener comas.\n");
    }
}

// mostrar_menu
void mostrar_menu(const char *version, int limite)
{
    char texto_limite[32];

    if (limite == 0) {
        snprintf(texto_limite, sizeof texto_limite, "sin limite");
    } else {
        snprintf(texto_limite, sizeof texto_limite, "%d", limite);
    }

    printf("\n==========================================================\n");
    printf("  Consulta de productos  |  IPC: %-10s | limite: %s\n",
           version, texto_limite);
    printf("==========================================================\n");
    printf("  1. Buscar por categoria\n");
    printf("  2. Buscar por rango de precio\n");
    printf("  3. Buscar por nombre (coincidencia parcial)\n");
    printf("  4. Cambiar límite de resultados mostrados\n");
    printf("  5. Salir y detener el servidor\n");
    printf("  0. Salir\n");
}

// construir_solicitud: traduce la opción del menú a una solicitud_t.
int construir_solicitud(int opcion, int limite, solicitud_t *solicitud)
{
    memset(solicitud, 0, sizeof *solicitud);
    solicitud->magia = MAGIA_PROTOCOLO;
    solicitud->pid_cliente = (int32_t) getpid();
    solicitud->limite = limite;

    switch (opcion) {
    case OPCION_CATEGORIA:
        solicitud->operacion = OP_BUSCAR_CATEGORIA;
        printf("  Categorías del dataset de ejemplo: %s\n", CATEGORIAS_EJEMPLO);
        return leer_texto_busqueda("  Categoria: ", solicitud->texto,
                                   sizeof solicitud->texto);
    case OPCION_PRECIO:
        solicitud->operacion = OP_BUSCAR_PRECIO;
        if (leer_real("  Precio minimo: ", 0.0, &solicitud->precio_min) == -1) {
            return -1;
        }
        return leer_real("  Precio maximo: ", solicitud->precio_min,
                         &solicitud->precio_max);
    case OPCION_NOMBRE:
        solicitud->operacion = OP_BUSCAR_NOMBRE;
        return leer_texto_busqueda("  Texto a buscar en el nombre: ",
                                   solicitud->texto, sizeof solicitud->texto);
    case OPCION_SALIR:
        solicitud->operacion = OP_DESCONECTAR;
        return 0;
    case OPCION_DETENER:
        solicitud->operacion = OP_TERMINAR;
        return 0;
    default:
        return -1;
    }
}

// Impresion de resultados

void imprimir_encabezado_resultados(void)
{
    printf("\n%-8s %-28s %-14s %10s %6s %-10s\n",
           "ID", "NOMBRE", "CATEGORIA", "PRECIO", "STOCK", "FECHA");
    printf("-------- ---------------------------- -------------- "
           "---------- ------ ----------\n");
}

void imprimir_registro(const registro_t *registro)
{
    printf("%-8lld %-28.28s %-14.14s %10.2f %6d %-10s\n",
           (long long) registro->id, registro->nombre, registro->categoria,
           registro->precio, (int) registro->stock, registro->fecha);
}

void imprimir_resumen(const resumen_t *resumen)
{
    if (resumen->coincidencias == 0) {
        printf("\nNo se encontraron resultados\n");
    } else {
        printf("\nCoincidencias: %d | mostradas: %d",
               (int) resumen->coincidencias, (int) resumen->enviados);
        if (resumen->enviados < resumen->coincidencias) {
            printf("  (limite alcanzado; use la opcion 4 para cambiarlo)");
        }
        printf("\n");
    }
    printf("Servidor: %lld lineas leidas del CSV, %d invalidas, %.2f ms\n",
           (long long) resumen->lineas_leidas, (int) resumen->lineas_invalidas,
           resumen->milisegundos);
}

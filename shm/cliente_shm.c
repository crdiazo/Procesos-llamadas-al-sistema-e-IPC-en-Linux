
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun/interfaz.h"
#include "../comun/protocolo.h"
#include "../comun/semaforos.h"
#include "../comun/utilidades.h"

static volatile sig_atomic_t terminar_cliente = 0;

typedef struct {
    int                  fd_memoria;
    region_compartida_t *region;
    sem_t               *sem_turno;
    sem_t               *sem_solicitud;
    sem_t               *sem_respuesta;
    sem_t               *sem_consumido;
    pid_t                pid_servidor;  
    registro_t          *copia_bloque;  
} conexion_shm_t;

typedef enum {
    CONSULTA_COMPLETA =  0,
    CONSULTA_ADIOS    =  1,
    CONSULTA_FALLO    = -1
} resultado_consulta_t;

static void manejar_senal_terminacion(int numero_senal)
{
    (void) numero_senal;
    terminar_cliente = 1;
}

static sem_t *abrir_semaforo(const char *nombre)
{
    sem_t *semaforo = sem_open(nombre, 0);

    if (semaforo == SEM_FAILED) {
        fprintf(stderr, "sem_open(%s): %s\n", nombre, strerror(errno));
        return NULL;
    }
    return semaforo;
}

static void cerrar_semaforo(sem_t **semaforo, const char *nombre)
{
    if (*semaforo != NULL) {
        if (sem_close(*semaforo) == -1) {
            fprintf(stderr, "sem_close(%s): %s\n", nombre, strerror(errno));
        }
        *semaforo = NULL;
    }
}


static void desconectar(conexion_shm_t *conexion)
{
    free(conexion->copia_bloque);
    conexion->copia_bloque = NULL;

    cerrar_semaforo(&conexion->sem_turno, SEM_TURNO);
    cerrar_semaforo(&conexion->sem_solicitud, SEM_SOLICITUD);
    cerrar_semaforo(&conexion->sem_respuesta, SEM_RESPUESTA);
    cerrar_semaforo(&conexion->sem_consumido, SEM_CONSUMIDO);

    if (conexion->region != NULL) {
        if (munmap(conexion->region, sizeof *conexion->region) == -1) {
            perror("munmap");
        }
        conexion->region = NULL;
    }
    cerrar_descriptor(conexion->fd_memoria, "memoria compartida");
    conexion->fd_memoria = -1;
}

static int conectar_con_servidor(conexion_shm_t *conexion)
{
    struct stat informacion;
    void *direccion;

    memset(conexion, 0, sizeof *conexion);
    conexion->fd_memoria = -1;

    conexion->fd_memoria = shm_open(SHM_NOMBRE, O_RDWR, 0);
    if (conexion->fd_memoria == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: no existe la memoria compartida %s. ¿Está en "
                    "ejecución servidor_shm?\n", SHM_NOMBRE);
        } else {
            perror("shm_open(" SHM_NOMBRE ")");
        }
        return -1;
    }
    if (fstat(conexion->fd_memoria, &informacion) == -1) {
        perror("fstat(memoria compartida)");
        return -1;
    }
    if ((size_t) informacion.st_size < sizeof(region_compartida_t)) {
        fprintf(stderr, "Error: la región mide %lld bytes (se esperaban %zu); el "
                "servidor no está inicializado o es de otra versión.\n",
                (long long) informacion.st_size, sizeof(region_compartida_t));
        return -1;
    }

    direccion = mmap(NULL, sizeof(region_compartida_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, conexion->fd_memoria, 0);
    if (direccion == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    conexion->region = direccion;

    if (conexion->region->magia != MAGIA_PROTOCOLO || !conexion->region->servidor_activo) {
        fprintf(stderr, "Error: el servidor no está listo o ya terminó.\n");
        return -1;
    }
    conexion->pid_servidor = (pid_t) conexion->region->pid_servidor;
    if (!proceso_existe(conexion->pid_servidor)) {
        fprintf(stderr, "Error: el servidor (PID %d) ya no existe; quedaron recursos "
                "IPC huérfanos. Ejecute 'make limpiar-ipc' o reinicie "
                "servidor_shm.\n", (int) conexion->pid_servidor);
        return -1;
    }

    if ((conexion->sem_turno = abrir_semaforo(SEM_TURNO)) == NULL ||
        (conexion->sem_solicitud = abrir_semaforo(SEM_SOLICITUD)) == NULL ||
        (conexion->sem_respuesta = abrir_semaforo(SEM_RESPUESTA)) == NULL ||
        (conexion->sem_consumido = abrir_semaforo(SEM_CONSUMIDO)) == NULL) {
        return -1;
    }

    /* Búfer local para copiar cada bloque de resultados */
    conexion->copia_bloque = malloc(sizeof(registro_t) * REGISTROS_POR_BLOQUE);
    if (conexion->copia_bloque == NULL) {
        perror("malloc");
        return -1;
    }
    return 0;
}

static void informar_fallo_espera(resultado_espera_t resultado, pid_t pid_servidor,
                                  const char *que_se_esperaba)
{
    switch (resultado) {
    case ESPERA_PROCESO_AUSENTE:
        fprintf(stderr, "Error: el servidor (PID %d) terminó mientras se esperaba "
                "%s.\n", (int) pid_servidor, que_se_esperaba);
        break;
    case ESPERA_INTERRUMPIDA:
        fprintf(stderr, "\nEspera de %s interrumpida por señal.\n", que_se_esperaba);
        break;
    case ESPERA_ERROR:
    case ESPERA_OK:
    default:
        break; 
    }
}

static resultado_consulta_t realizar_consulta(conexion_shm_t *conexion,
                                              const solicitud_t *solicitud)
{
    region_compartida_t *region = conexion->region;
    resultado_espera_t espera;
    int encabezado_impreso = 0;

    espera = esperar_semaforo_vigilado(conexion->sem_turno, conexion->pid_servidor,
                                       &terminar_cliente, SEGUNDOS_VIGILANCIA);
    if (espera != ESPERA_OK) {
        informar_fallo_espera(espera, conexion->pid_servidor, "el turno");
        return CONSULTA_FALLO;
    }

    region->solicitud = *solicitud;
    region->estado = EST_SOLICITUD;
    if (sem_post(conexion->sem_solicitud) == -1) {
        perror("sem_post(solicitud)");
        if (sem_post(conexion->sem_turno) == -1) {
            perror("sem_post(turno)");
        }
        return CONSULTA_FALLO;
    }

    for (;;) {
        int32_t estado;
        int32_t num_registros;
        resumen_t resumen;
        char mensaje[TAM_MENSAJE_ERROR];
        int32_t i;

        espera = esperar_semaforo_vigilado(conexion->sem_respuesta,
                                           conexion->pid_servidor,
                                           &terminar_cliente, SEGUNDOS_VIGILANCIA);
        if (espera != ESPERA_OK) {
            informar_fallo_espera(espera, conexion->pid_servidor, "la respuesta");
            return CONSULTA_FALLO;
        }

        estado = region->estado;
        num_registros = region->num_registros;
        if (num_registros < 0 || num_registros > REGISTROS_POR_BLOQUE) {
            fprintf(stderr, "Error de protocolo: num_registros = %d.\n",
                    (int) num_registros);
            num_registros = 0;
            estado = -1;
        }
        memcpy(conexion->copia_bloque, region->registros,
               (size_t) num_registros * sizeof(registro_t));
        resumen = region->resumen;
        copiar_cadena(mensaje, sizeof mensaje, region->mensaje_error);

        if (sem_post(conexion->sem_consumido) == -1) {
            perror("sem_post(consumido)");
            return CONSULTA_FALLO;
        }

        switch (estado) {
        case EST_BLOQUE:
        case EST_FIN:
            for (i = 0; i < num_registros; i++) {
                if (!encabezado_impreso) {
                    imprimir_encabezado_resultados();
                    encabezado_impreso = 1;
                }
                imprimir_registro(&conexion->copia_bloque[i]);
            }
            if (estado == EST_FIN) {
                imprimir_resumen(&resumen);
                return CONSULTA_COMPLETA;
            }
            break; 
        case EST_ERROR:
            fprintf(stderr, "El servidor informó un error: %s\n", mensaje);
            return CONSULTA_COMPLETA;
        case EST_ADIOS:
            return CONSULTA_ADIOS;
        default:
            fprintf(stderr, "Error de protocolo: estado desconocido (%d).\n",
                    (int) estado);
            return CONSULTA_FALLO;
        }
    }
}

int main(void)
{
    conexion_shm_t conexion;
    int limite = LIMITE_POR_DEFECTO;
    int codigo_salida = EXIT_SUCCESS;
    int continuar = 1;

    if (instalar_manejadores_terminacion(manejar_senal_terminacion) == -1) {
        return EXIT_FAILURE;
    }
    printf("[cliente_shm] PID=%d PPID=%d\n", (int) getpid(), (int) getppid());

    if (conectar_con_servidor(&conexion) == -1) {
        desconectar(&conexion);
        return EXIT_FAILURE;
    }
    printf("[cliente_shm] Conectado a %s (servidor PID %d, fd=%d, región en %p)\n",
           SHM_NOMBRE, (int) conexion.pid_servidor, conexion.fd_memoria,
           (void *) conexion.region);

    while (continuar && !terminar_cliente) {
        long opcion;
        solicitud_t solicitud;

        mostrar_menu("SHM", limite);
        if (leer_entero("Seleccione una opción: ", 0, OPCION_MAXIMA, &opcion) == -1) {
            break;
        }

        if (opcion == OPCION_LIMITE) {
            long nuevo_limite;
            if (leer_entero("Nuevo límite (0 = sin límite): ", 0, 1000000,
                            &nuevo_limite) == 0) {
                limite = (int) nuevo_limite;
            }
            continue;
        }

        if (construir_solicitud((int) opcion, limite, &solicitud) == -1) {
            break;
        }

        switch (realizar_consulta(&conexion, &solicitud)) {
        case CONSULTA_COMPLETA:
            break;
        case CONSULTA_ADIOS:
            printf("[cliente_shm] El servidor confirmó %s.\n",
                   opcion == OPCION_DETENER ? "su terminación" : "la desconexión");
            continuar = 0;
            break;
        case CONSULTA_FALLO:
            codigo_salida = EXIT_FAILURE;
            continuar = 0;
            break;
        }
    }

    if (terminar_cliente) {
        printf("\n[cliente_shm] Señal recibida: cerrando ordenadamente.\n");
    }
    desconectar(&conexion);
    printf("[cliente_shm] Recursos cerrados (munmap, close, sem_close). Fin.\n");
    return codigo_salida;
}

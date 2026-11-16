#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "virtualizacion.h"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

static void leer_dmi(const char *campo, char *buf, size_t n) {
    char ruta[128];
    snprintf(ruta, sizeof ruta, "/sys/class/dmi/id/%s", campo);
    FILE *f = fopen(ruta, "r");
    snprintf(buf, n, "(no disponible)");
    if (!f) return;
    if (fgets(buf, (int)n, f)) buf[strcspn(buf, "\r\n")] = '\0';
    fclose(f);
}

static void detectar_ept_npt(int *ept, int *npt) {
    *ept = 0;
    *npt = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char linea[1024];
    while (fgets(linea, sizeof linea, f)) {
        if (!strstr(linea, "flags") && !strstr(linea, "Features")) continue;
        char *token = strtok(linea, ": \t\r\n");
        while (token) {
            if (strcmp(token, "ept") == 0) *ept = 1;
            if (strcmp(token, "npt") == 0) *npt = 1;
            token = strtok(NULL, ": \t\r\n");
        }
    }
    fclose(f);
}

static void leer_systemd_virt(char *buf, size_t n) {
    snprintf(buf, n, "no disponible");
    FILE *p = popen("systemd-detect-virt 2>/dev/null", "r");
    if (!p) return;
    if (fgets(buf, (int)n, p)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0] == '\0') snprintf(buf, n, "ninguno / no detectado");
    }
    (void)pclose(p);
}

static const char *nombre_hipervisor(const char *firma, const char *systemd) {
    if (strstr(firma, "VBox") || strcmp(systemd, "oracle") == 0) return "VirtualBox";
    if (strstr(firma, "VMware") || strcmp(systemd, "vmware") == 0) return "VMware";
    if (strstr(firma, "KVM") || strcmp(systemd, "kvm") == 0 || strcmp(systemd, "qemu") == 0) return "KVM/QEMU";
    if (strstr(firma, "Microsoft") || strcmp(systemd, "microsoft") == 0) return "Hyper-V";
    if (strstr(firma, "Xen") || strcmp(systemd, "xen") == 0) return "Xen";
    if (strcmp(systemd, "none") == 0) return "hardware físico (sin VM detectada)";
    if (strstr(systemd, "container") || strcmp(systemd, "docker") == 0 ||
        strcmp(systemd, "podman") == 0 || strcmp(systemd, "lxc") == 0 ||
        strcmp(systemd, "systemd-nspawn") == 0) return "no identificado (entorno contenedor detectado)";
    if (firma[0]) return firma;
    return "no identificado";
}

static const char *tipo_hipervisor(const char *nombre) {
    if (strcmp(nombre, "VirtualBox") == 0) return "tipo 2 (alojado)";
    if (strcmp(nombre, "KVM/QEMU") == 0 || strcmp(nombre, "Hyper-V") == 0 || strcmp(nombre, "Xen") == 0)
        return "tipo 1 (bare-metal / integrado en el kernel)";
    if (strcmp(nombre, "VMware") == 0)
        return "depende del producto: Workstation tipo 2; ESXi tipo 1";
    return "no determinado con los datos expuestos";
}

#if !defined(__x86_64__) && !defined(__i386__)
static void mostrar_lscpu(void) {
    FILE *p = popen("lscpu 2>/dev/null", "r");
    if (!p) {
        printf("lscpu: no disponible\n");
        return;
    }
    char linea[256];
    while (fgets(linea, sizeof linea, p)) {
        if (strncmp(linea, "Architecture:", 13) == 0 ||
            strncmp(linea, "Vendor ID:", 10) == 0 ||
            strncmp(linea, "Model name:", 11) == 0 ||
            strncmp(linea, "Virtualization:", 15) == 0 ||
            strncmp(linea, "Hypervisor vendor:", 18) == 0 ||
            strncmp(linea, "Virtualization type:", 20) == 0)
            printf("  %s", linea);
    }
    (void)pclose(p);
}
#endif

void virt_info(void) {
    char cpu_vendor[13] = "no disponible";
    char firma[13] = "";
    char dmi_vendor[96], dmi_producto[96], systemd[64];
    int dentro_vm = 0, vtx = 0, amdv = 0;
    int ept = 0, npt = 0;

    printf("\n=========== INFORMACIÓN DE VIRTUALIZACIÓN ===========\n");
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a = 0, b = 0, c = 0, d = 0;
    unsigned int max_basica = __get_cpuid_max(0, NULL);
    if (__get_cpuid(0, &a, &b, &c, &d)) {
        memcpy(cpu_vendor, &b, 4);
        memcpy(cpu_vendor + 4, &d, 4);
        memcpy(cpu_vendor + 8, &c, 4);
        cpu_vendor[12] = '\0';
    }
    if (max_basica >= 1 && __get_cpuid(1, &a, &b, &c, &d)) {
        dentro_vm = (int)((c >> 31) & 1U);
        vtx = (int)((c >> 5) & 1U);
    }

    unsigned int max_extendida = __get_cpuid_max(0x80000000U, NULL);
    if (max_extendida >= 0x80000001U && __get_cpuid(0x80000001U, &a, &b, &c, &d))
        amdv = (int)((c >> 2) & 1U);

    unsigned int max_hipervisor = __get_cpuid_max(0x40000000U, NULL);
    if (max_hipervisor >= 0x40000000U && __get_cpuid(0x40000000U, &a, &b, &c, &d)) {
        memcpy(firma, &b, 4);
        memcpy(firma + 4, &c, 4);
        memcpy(firma + 8, &d, 4);
        firma[12] = '\0';
    }
#else
    printf("Arquitectura no x86: CPUID no está disponible; se consultará lscpu.\n");
    mostrar_lscpu();
#endif

    detectar_ept_npt(&ept, &npt);
    leer_dmi("sys_vendor", dmi_vendor, sizeof dmi_vendor);
    leer_dmi("product_name", dmi_producto, sizeof dmi_producto);
    leer_systemd_virt(systemd, sizeof systemd);

    const char *hipervisor = nombre_hipervisor(firma, systemd);
    printf("Fabricante CPU (CPUID):       %s\n", cpu_vendor);
    printf("¿Dentro de una VM?:           %s\n", dentro_vm ? "SÍ" : "NO DETECTADO");
    printf("Firma del hipervisor:         %s\n", firma[0] ? firma : "no disponible");
    printf("Intel VT-x / VMX expuesto:    %s\n", vtx ? "SÍ" : "NO");
    printf("AMD-V / SVM expuesto:         %s\n", amdv ? "SÍ" : "NO");
    printf("EPT disponible (/proc/cpuinfo): %s\n", ept ? "SÍ" : "NO");
    printf("NPT disponible (/proc/cpuinfo): %s\n", npt ? "SÍ" : "NO");
    printf("Fabricante DMI:               %s\n", dmi_vendor);
    printf("Producto DMI:                 %s\n", dmi_producto);
    printf("systemd-detect-virt:          %s\n", systemd);
    printf("Tipo de hipervisor:           %s\n", tipo_hipervisor(hipervisor));

    printf("\nConclusión: ");
    if (dentro_vm)
        printf("MiniOS corre dentro de una VM (%s, %s), sobre una CPU %s.\n",
               hipervisor, tipo_hipervisor(hipervisor), cpu_vendor);
    else if (strcmp(systemd, "none") != 0 && strcmp(systemd, "no disponible") != 0)
        printf("systemd detecta el entorno %s; CPUID no confirmó el bit hypervisor. CPU %s.\n",
               systemd, cpu_vendor);
    else
        printf("no se detectó una máquina virtual; el entorno parece físico o no expone la virtualización. CPU %s.\n",
               cpu_vendor);
    printf("======================================================\n");
}

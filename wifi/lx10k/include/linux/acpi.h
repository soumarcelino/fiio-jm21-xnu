/* JM21: no ACPI; every evaluation fails, so ath10k uses its defaults */
#ifndef LX10K_ACPI_H
#define LX10K_ACPI_H
typedef void *acpi_handle;
typedef int acpi_status;
typedef char *acpi_string;
#define ACPI_TYPE_INTEGER 1
#define ACPI_TYPE_PACKAGE 4
#define ACPI_ALLOCATE_BUFFER ((size_t)-1)
#define ACPI_FAILURE(s) ((s) != 0)
#define ACPI_HANDLE(d) ((acpi_handle)NULL)
#define ACPI_COMPANION(d) NULL
struct acpi_buffer { size_t length; void *pointer; };
union acpi_object {
	u32 type;
	struct { u32 type; u64 value; } integer;
	struct { u32 type; u32 count; union acpi_object *elements; } package;
};
static inline acpi_status acpi_get_handle(acpi_handle p, const char *n, acpi_handle *r) { return 1; }
static inline acpi_status acpi_evaluate_object(acpi_handle h, const char *n, void *a, struct acpi_buffer *b) { return 1; }
#endif

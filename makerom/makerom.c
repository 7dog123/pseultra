/*
 * pseultra/makerom/makerom.c
 * ROM creator tool
 *
 * (C) pseudophpt 2018
 */

#include "makerom.h"
#include "elf.h"
#include <util.h>
#include <n64.h>
#include <elf.h>
#include <string.h>
#include <stdlib.h>

segment *segments;
int segment_count = 0;

int main (int argc, char *argv[]) {
    FILE* file;
    FILE *lscript;

    if (argc != 5) {
        printf("Usage: makerom <spec file> <ld path> <boot file> <output>\n");
        return 0;
    }

    // Open spec file
    file = fopen(argv[1], "r");

    // Process it
    process_specfile(file);
    
    // Close it
    fclose(file);

    // Generate rom layout
    calculate_rom_layout();

    // Open linker script
    lscript = fopen("link.ld", "w");

    // Generate it
    generate_linker_script(lscript);

    // Close it
    fclose(lscript);

    // Link
    char *link_command = malloc(sizeof(char) * strlen(argv[2]) + 15);
    sprintf(link_command, "%s -T link.ld -G0", argv[2]);
    system(link_command);

    // Create rom image
    make_rom(argv[4], argv[3]);
}

void process_specfile (FILE *file) {
    char segment_name [16];
    char objfile_name [256];

    segments = NULL;

    // Get segment name
    while(fscanf(file, " %15s ", segment_name) == 1) {
        segment seg = create_segment(segment_name);

        // Get included object files
        while(fscanf(file, " %255[^\n ;] ", objfile_name) == 1) {
            // Create section
            section sec = create_section(objfile_name);

            // Add to segment
            add_section(sec, &seg);
        }

        add_segment(seg);

        if (fscanf(file, " ; ") == EOF) break;
    }
}

void calculate_rom_layout () {
    uint32_t rom_location_counter = 0x1000;

    for (int i = 0; i < segment_count; i ++) {
        uint32_t segment_size = 0;

        // Get size of .text and .data for each section
        for (int j = 0; j < segments[i].section_count; j ++) {
            elf32_shentry text = get_section (segments[i].sections[j].buffer, ".text");
            elf32_shentry data = get_section (segments[i].sections[j].buffer, ".data");

            segment_size += BE_TO_LE32(text.size) + BE_TO_LE32(data.size);
        }

        segments[i].rom_start = rom_location_counter;
        
        rom_location_counter += segment_size;
        
        segments[i].rom_end = rom_location_counter;
    }
}

void generate_linker_script (FILE *lscript) {
    // Notice
    fprintf(lscript, "/* This linker script was automatically generated by the pseultra makerom tool */\n");
    
    // Entry point
    fprintf(lscript, "ENTRY(_boot)\n");

    // INPUTs
    fprintf(lscript, "INPUT(\n");
    for (int i = 0; i < segment_count; i ++) {
        for (int j = 0; j < segments[i].section_count; j ++) {
            fprintf(lscript, "\t%s\n", segments[i].sections[j].filename);
        }
    }
    fprintf(lscript, ")\n");

    // OUTPUT
    fprintf(lscript, "OUTPUT(rom.elf)\n");

    // SECTIONS
    fprintf(lscript, "SECTIONS {\n");

    // Initial RAM location
    fprintf(lscript, "\t. = 0x80001000;\n");

    for (int i = 0; i < segment_count; i ++) {
        segment seg = segments[i];

        // Symbols for ROM start and end
        fprintf(lscript, "\t_%sSegmentRomStart = 0x%x;\n", seg.name, seg.rom_start);
        
        fprintf(lscript, "\t_%sSegmentRomEnd = 0x%x;\n\n", seg.name, seg.rom_end);
    
        // Text and data section
        fprintf(lscript, "\t_%sSegmentTextStart = ABSOLUTE(.);\n\n", seg.name);

        fprintf(lscript, "\t.%s.text : {\n", seg.name);

        // Add each section's text and data sectionns
        for (int j = 0; j < seg.section_count; j ++) {
            section sec = seg.sections[j];

            fprintf(lscript, "\t\t%s (.text)\n", sec.filename);
            fprintf(lscript, "\t\t%s (.data)\n", sec.filename);
        }

        fprintf(lscript, "\t}\n\n");

        fprintf(lscript, "\t_%sSegmentTextEnd = ABSOLUTE(.);\n\n", seg.name);

        // BSS section 
        fprintf(lscript, "\t_%sSegmentBssStart = ABSOLUTE(.);\n\n", seg.name);

        fprintf(lscript, "\t.%s.bss : {\n", seg.name);

        // Add each section's text and data sectionns
        for (int j = 0; j < seg.section_count; j ++) {
            section sec = seg.sections[j];

            fprintf(lscript, "\t\t%s (.bss)\n", sec.filename);
            fprintf(lscript, "\t\t%s (COMMON)\n", sec.filename);
        }

        fprintf(lscript, "\t}\n\n");

        fprintf(lscript, "\t_%sSegmentBssEnd = ABSOLUTE(.);\n\n", seg.name);

   }

    // Discard symbols
    fprintf(lscript, "\t/DISCARD/ : {\n");

    fprintf(lscript, "\t\t* (.MIPS.abiflags)\n");
    fprintf(lscript, "\t\t* (.pdr)\n");
    fprintf(lscript, "\t\t* (.comment)\n");
    fprintf(lscript, "\t\t* (.reginfo)\n");
    fprintf(lscript, "\t\t* (.gnu.attributes)\n");

    fprintf(lscript, "\t}\n");
    fprintf(lscript, "}\n");
}

void make_rom (char *rom_name, char *bootcode) {
    // Open ROM
    FILE *rom = fopen(rom_name, "w");
    
    // Open bootcode file
    char *bootcode_buffer = open_file(bootcode);

    // Open ROM ELF
    char *rom_elf_buffer = open_file("rom.elf");

    // Write header parameters
    elf32_header *rom_elf_header = (elf32_header *)(rom_elf_buffer);

    rom_header *rom_head = (rom_header *)bootcode_buffer; 

    rom_head->pi_regs = PI_VALUES;
    rom_head->boot_address = rom_elf_header->entry; // Entry point
    rom_head->rom_start = LE_TO_BE32(segments[0].rom_start); // Start of boot segment
    rom_head->rom_length = LE_TO_BE32(segments[0].rom_end - segments[0].rom_start); // Boot segment length
    
    // Write the bootcode to the file
    fwrite (bootcode_buffer, 0x1000, 1, rom);

    // Write each segment to the file
    for (int i = 0; i < segment_count; i ++) {
        // Text section name
        char *section_name = malloc(strlen(segments[i].name) + 7);

        sprintf(section_name, ".%s.text", segments[i].name);

        elf32_shentry section_header = get_section(rom_elf_buffer, section_name);

        printf(section_name);

        // Section location
        void *segment_section = (void *)(rom_elf_buffer + BE_TO_LE32(section_header.offset));

        // Write from ELF to ROM
        fwrite(segment_section, BE_TO_LE32(section_header.size), 1, rom);
        
        // Free string
        free(section_name);
    }
}

// Copies the string to a new section and opens the buffer
section create_section (char *filename) {
    section rsec;

    // Allocate memory for name
    rsec.filename = malloc(strlen(filename));

    // Copy it over
    strcpy(rsec.filename, filename);

    // Open file
    rsec.buffer = open_file(filename);

    // Returns
    return rsec;
}

segment create_segment (char *name) {
    segment rseg;

    // Allocate memory for name
    rseg.name = malloc(strlen(name));

    // Copy it over
    strcpy(rseg.name, name);

    // Fill in rest of parameters
    rseg.sections = malloc(0);

    rseg.section_count = 0;

    // Returns
    return rseg;
}

void add_section (section sec, segment *seg) {
    seg->section_count ++;
    seg->sections = realloc(seg->sections, seg->section_count * sizeof(section));
    seg->sections[seg->section_count - 1] = sec;
}

void add_segment (segment seg) {
    segment_count ++;
    segments = realloc(segments, segment_count * sizeof(segment));
    segments[segment_count - 1] = seg;
}

char *open_file (char *filename) {
    FILE* file;

    // Open file
    file = fopen(filename, "r");
    
    // Get position
    fseek(file, 0, SEEK_END); // Go to end
    int size = ftell(file); // Get position
    fseek(file, 0, SEEK_SET); // Go to beginning

    char *buffer = malloc (size);

    // Read into buffer
    fread(buffer, size, 1, file);

    // Close
    fclose(file);
    
    // Return
    return buffer;
}


void close_file (char *buffer) {
    free(buffer);
}

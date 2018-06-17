/*
 * redis.c
 *
 *  Created on: 8 jun. 2018
 *      Author: utnso
 */
#define _GNU_SOURCE
#include "redis.h"
#include <commons/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/mman.h>
#include <sys/types.h>
#include <fcntl.h>


int effective_position_for(t_redis* redis, int position){
	return position % redis->number_of_entries;
}

/*
 * Replaces all the atomic keys needed to fit the value_size.
 * Compaction may be needed afterwards to ensure that memory is contiguous.
 * PRE: the value_size must be less than the memory size.
 */
void redis_replace_circular(struct Redis* redis, unsigned int value_size){
	int slot_cursor = redis->current_slot;
	unsigned int freed_memory = 0;

	t_memory_position* mem_pos;
	t_entry_data* entry_data;

	int required_slots = slots_occupied_by(redis->entry_size, value_size);

	while(redis->slots_available < required_slots &&  slot_cursor < redis->number_of_entries + redis->current_slot){
		int pos = effective_position_for(redis, slot_cursor);

		mem_pos = redis->occupied_memory_map[pos];
		if(mem_pos->used && mem_pos->is_atomic){
			entry_data = dictionary_get(redis->key_dictionary, mem_pos->key);

			freed_memory ++;
			redis_remove_key(redis, mem_pos->key, entry_data, 1); // 1 because it is atomic
		}
		slot_cursor++;
	}
}


void redis_destroy(t_redis* redis){
	if(redis == NULL) return;

	if(redis->memory_region != NULL)
		free(redis->memory_region);

	if(redis->occupied_memory_map != NULL){
		for(int i=0; i<redis->number_of_entries; i++){
			if(redis->occupied_memory_map[i] != NULL) free(redis->occupied_memory_map[i]);
		}
		free(redis->occupied_memory_map);
	}

	dictionary_destroy_and_destroy_elements(redis->key_dictionary, redis_entry_data_destroy);

	if(redis->mount_dir != NULL)
		free(redis->mount_dir);

	free(redis);
}

t_memory_position* redis_create_empty_memory_position(){
	t_memory_position* memory_pos = malloc(sizeof(t_memory_position));
	memory_pos->is_atomic = true;
	memory_pos->used = false;
	memory_pos->last_reference = 0;
	memory_pos->key[0] = '\0';

	return memory_pos;
}

t_redis* redis_init(int entry_size, int number_of_entries, t_log* log, const char* mount_dir,
		void (*replace_necessary_positions)(struct Redis*, unsigned int)){
	t_redis* redis = malloc(sizeof(t_redis));
	redis->entry_size = entry_size;
	redis->number_of_entries = number_of_entries;
	redis->storage_size = entry_size * number_of_entries;

	redis->current_slot = 0;
	redis->memory_region = malloc(redis->storage_size);
	redis->occupied_memory_map = malloc(number_of_entries * sizeof(t_memory_position*));
	for(int i = 0; i < number_of_entries; i++){
		redis->occupied_memory_map[i] = redis_create_empty_memory_position();
	}

	redis->key_dictionary = dictionary_create();
	redis->log = log;

	redis->mount_dir = string_duplicate(mount_dir);
	redis->replace_necessary_positions = replace_necessary_positions;

	redis->slots_available = number_of_entries;

	return redis;
}

void redis_entry_data_destroy(t_entry_data* entry_data){
	if(entry_data == NULL) return;

	if(entry_data->mapped_value != NULL){
		if (munmap(entry_data->mapped_value, entry_data->size) == -1){
			perror("Error unmapping data file!");
		}
	}

	if(entry_data->mapped_file != NULL){
		fclose(entry_data->mapped_file);
	}

	free(entry_data);
}

char* redis_get(t_redis* redis, char* key){
	if(!dictionary_has_key(redis->key_dictionary, key)){
		return NULL;
	}

	t_entry_data* entry_data = (t_entry_data*)dictionary_get(redis->key_dictionary, key);
	char* stored_value = malloc(entry_data->size);

	int offset = redis->entry_size * entry_data->first_position;

	log_debug(redis->log, "Key: %s is stored at: %i. Size: %i. Calculated offset: %i",
			key, entry_data->first_position, entry_data->size, offset);

	memcpy(stored_value, redis->memory_region + offset, entry_data->size);

	return stored_value;
}


int slots_occupied_by(int entry_size, int value_size){
	int slots = value_size / entry_size;
	if(value_size % entry_size > 0) slots++;

	return slots;
}

void redis_free_slot(t_redis* redis, int slot_index){
	t_memory_position* mem_pos = redis->occupied_memory_map[slot_index];
	mem_pos->is_atomic = true;
	mem_pos->key[0] = '\0';
	mem_pos->last_reference = 0;
	mem_pos->used = false;
}

void set_in_same_place(t_redis* redis, t_entry_data* entry_data, char* key, char* value,
		int value_size, int needed_slots, int used_slots){

	int slots_to_free = used_slots - needed_slots;

	int slot_index = entry_data->first_position + needed_slots;
	while(slots_to_free > 0){
		redis_free_slot(redis, slot_index);
		slot_index++;
		slots_to_free--;
		redis->slots_available++;
	}

	// set atomic if necessary
	if(needed_slots == 1){
		redis->occupied_memory_map[entry_data->first_position]->is_atomic = true;
	}

	// remap memory file if needed
	if(is_memory_mapped(entry_data)){
		// resize the mapped region to the actual size
		void *temp = mremap(entry_data->mapped_value, entry_data->size, value_size, MREMAP_MAYMOVE);
		if(temp == (void*)-1)
		{
			log_error(redis->log, "FATAL ERROR: Could not remap memory for key: %s", key);
			exit_program(EXIT_FAILURE);
		}

		entry_data->mapped_value = temp;

		memcpy(entry_data->mapped_value, value, value_size);
	}


	// copy the new value
	int offset = entry_data->first_position * redis->entry_size;
	memcpy(redis->memory_region + offset, value, value_size);
	entry_data->size = value_size;
}

// TODO: Repite codigo con set_in_same_place. Refactor!
void redis_remove_key(t_redis* redis, char* key, t_entry_data* entry_data, int used_slots){
	char * copied_key = string_duplicate(key);
	int slot_index = entry_data->first_position;
	int slots_to_free = used_slots;

	while(slots_to_free > 0){
		redis_free_slot(redis, slot_index);
		slot_index++;
		slots_to_free--;
		redis->slots_available++;
	}

	dictionary_remove_and_destroy(redis->key_dictionary, copied_key, redis_entry_data_destroy);
	free(copied_key);
}

/*
 * Checks the memory slots for available space to fit the required size.
 * Returns the first slot of that available space, or -1 if ther is no space.
 */
int get_first_contiguous_free_slots(t_redis* redis, int required_slots){
	int contiguous_free_slots = 0;
	int current_first = -1;
	int cursor;
	t_memory_position* mem_pos;

	for(cursor = 0; cursor < redis->number_of_entries && contiguous_free_slots < required_slots; cursor++){
		mem_pos = redis->occupied_memory_map[cursor];
		if(mem_pos->used){
			current_first = -1;
			contiguous_free_slots = 0;
		} else{
			if(current_first >= 0){
				contiguous_free_slots++;
			} else {
				current_first = cursor;
				contiguous_free_slots = 1;
			}
		}
	}

	if(contiguous_free_slots == required_slots){
		return current_first;
	} else {
		return -1;
	}
}

void redis_set_in_position(t_redis* redis, char* key, char* value, unsigned int value_size, int first_slot, int need_slots){
	// create the new key for the dictionary
	t_entry_data* entry_data = malloc(sizeof(t_entry_data));
	entry_data->first_position = first_slot;
	entry_data->size = value_size;
	entry_data->mapped_file = NULL; // FD and mmapped file are created on STORE
	entry_data->mapped_value = NULL;  // or at startup from an existing file.

	// mark the slots as used by this key
	t_memory_position* mem_pos;
	bool isAtomic = need_slots == 1;

	for(int pos = first_slot; pos < first_slot + need_slots; pos++){
		mem_pos = redis->occupied_memory_map[pos];
		mem_pos->is_atomic = isAtomic;
		strcpy(mem_pos->key, key);
		mem_pos->last_reference = 0; // TODO: FALTA SETEAR UNA VARIABLE CON EL NRO DE ITERACION
		mem_pos->used = true;
	}

	// memcpy the new value
	int offset = first_slot * redis->entry_size;
	memcpy(redis->memory_region + offset, value, value_size);

	// save the new key in the dictionary
	dictionary_put(redis->key_dictionary, key, entry_data);


	// Set the current_slot to the next position after the entry that was just inserted.
	// if the size is exceeded, it returns to zero.
	// NOTE: a value is always stored in contiguous memory. That means that it will never be
	// split between the end of the memory region and the beginning of it. The first_position
	// must ensure that there is enough space after that position to store the value.
	redis->current_slot = (first_slot + need_slots) % redis->number_of_entries;
	redis->slots_available -= need_slots;
}

bool redis_set(t_redis* redis, char* key, char* value, unsigned int value_size){
	int need_slots = slots_occupied_by(redis->entry_size, value_size);

	// Check if key is already present
	if(dictionary_has_key(redis->key_dictionary, key)){
		t_entry_data* entry_data = (t_entry_data*)dictionary_get(redis->key_dictionary, key);
		int used_slots = slots_occupied_by(redis->entry_size, entry_data->size);

		if(need_slots <= used_slots){
			// If the new value fits in the previously reserved slots, use those slots
			set_in_same_place(redis, entry_data, key, value, value_size, need_slots, used_slots);
			redis_print_status(redis);
			return true;
		} else{
			// If it does not fit, remove the key, free the slots and check if it fits somewhere else.
			redis_remove_key(redis, key, entry_data, used_slots);
		}
	}

	// At this point the key is either new or it was removed because the new value did not fit.

	bool space_available = need_slots <= redis->slots_available;

	// if there is no space available, then free some with the replacement algorithm
	if(!space_available){
		log_info(redis->log, "There is not enough space. Executing replacement algorithm. Needed slots: %i. Available: %i",
			need_slots, redis->slots_available);
		redis->replace_necessary_positions(redis, value_size);
	} else {
		log_info(redis->log, "There is enough space. Checking if space is contiguous. Needed slots: %i. Available: %i",
			need_slots, redis->slots_available);
	}

	// then check if there are contiguous slots
	int first_slot = get_first_contiguous_free_slots(redis, need_slots);

	if(first_slot < 0){
		// Need to compact because there is space available but it is not contiguous
		log_info(redis->log, "Space available is not contiguous to SET the value with size: %i. Need to compact.",
			value_size);
		redis_print_status(redis);
		return false;
	}

	redis_set_in_position(redis, key, value, value_size, first_slot, need_slots);
	redis_print_status(redis);
	return true;
}

void print_dict_key(char* key, void* val){
	printf("'%s',", key);
}

void redis_print_status(t_redis* redis){
	printf("\n================================================================================================\n");
	printf("   INSTANCE STATUS\n");
	printf("================================================================================================\n");
	printf("  Entry size: %i. Max entries: %i, Storage size (bytes): %i.\n", redis->entry_size,
			redis->number_of_entries, redis->storage_size);
	printf("  Total entries: %i. Current slot: %i.\n", dictionary_size(redis->key_dictionary), redis->current_slot);
	printf("  Keys: ");
	dictionary_iterator(redis->key_dictionary, print_dict_key);
	printf("\n");

	printf("  Memory Map:\n\n");

	/*
	 * +-------------------------------------------------+------------------------------------------+
	 * | Pos  | Key                                      |  Value                                   |
	 * +-------------------------------------------------+------------------------------------------+
	 * | 0001 | 1234567890123456789012345678901234567890 | 1234567890123456789012345678901234567890 |
	 * | 0002 | FREE                                     |                                          |
	 * +------+------------------------------------------+------------------------------------------+
	 */

	printf("  +-------------------------------------------------+------------------------------------------+\n");
	printf("  | Pos  | Key                                      | Value                                    |\n");
	printf("  +-------------------------------------------------+------------------------------------------+\n");

	int offset;
	int value_size_to_copy = redis->entry_size > 40 ? 40 : redis->entry_size;
	char val_buffer[41];

	t_memory_position* mem_pos;

	for(int i=0; i < redis->number_of_entries; i++){
		mem_pos = redis->occupied_memory_map[i];

		if(mem_pos->used){
			offset = redis->entry_size * i;
		    memcpy(&val_buffer, redis->memory_region + offset, value_size_to_copy);
		    val_buffer[value_size_to_copy] = '\0';

			printf("  | %04d | %-40s | %-40s |\n", i, mem_pos->key, val_buffer);
		} else {
			printf("  | %04d | FREE                                     |                                          |\n", i);
		}
	}

	printf("  +-------------------------------------------------+------------------------------------------+\n\n");
	printf("================================================================================================\n");
}

bool is_memory_mapped(t_entry_data* entry){
	return entry->mapped_file != NULL && entry->mapped_value != NULL;
}

bool create_and_map_file_for_entry(t_redis* redis, t_entry_data* entry, char* key){
	int mode = 0x0777;

	char* filename = malloc(strlen(redis->mount_dir) + strlen(key) + 1);
	strcpy(filename, redis->mount_dir);
	strcat(filename, key);

	/* open/create the output file */

	entry->mapped_file = fopen(filename, "ab+");

	//if ((entry->file_descriptor = open(filename, O_RDWR | O_CREAT | O_TRUNC, mode )) < 0){
	if(entry->mapped_file == NULL) {
		log_error(redis->log, "Could not open file for key: %s, file: %s", key, filename);
		free(filename);
		return false;
	}

	ftruncate(fileno(entry->mapped_file), entry->size);

	free(filename);

	if ((entry->mapped_value = mmap(NULL, entry->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fileno(entry->mapped_file), 0)) == (caddr_t) -1) {
		log_error("FATAL ERROR: Could not perform mmap on file for key: %s", key);
		return false;
	}

	return true;
}

/*
 * Store the value in the file system.
 * Result can be:
 *  0: Store was successful.
 *  1: The key was invalid.
 * -1: The operation failed with an unexpected error.
 */
int redis_store(t_redis* redis, char* key){
	// Get the entry
	t_entry_data* entry = dictionary_get(redis->key_dictionary, key);

	if(entry == NULL){
		log_warning(redis->log, "Attempted STORE on inexistent key: %s.", key);
		return 1; // key not valid
	}

	// If the file is not present yet, create it and map the value.
	// If it's present, the value must already be copied.
	if(!is_memory_mapped(entry)){
		if(!create_and_map_file_for_entry(redis, entry, key)){
			log_error(redis->log, "FATAL ERROR: Could not create memory mapped file for key: %s.", key);
			return -1;
		}

		// copy the value to the memory mapped
		int offset = redis->entry_size * entry->first_position;
		memcpy(entry->mapped_value, redis->memory_region + offset, entry->size);
	}

	// flush to disk
	if (msync((void *)entry->mapped_value, entry->size, MS_SYNC) < 0) {
		log_error(redis->log, "FATAL ERROR: Could not sync memory mapped file for key: %s.", key);
		return -1;
	}

	return 0;
}


void redis_compact(t_redis* redis){


}

void redis_load_dump_files(t_redis* redis){
	// TODO! IMPLEMENTAR
}


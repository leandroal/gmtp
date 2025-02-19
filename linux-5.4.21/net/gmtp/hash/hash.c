/*
 * hash.c
 *
 *  Created on: 27/02/2015
 *      Author: mario
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../gmtp.h"
#include "hash.h"

int gmtp_build_hashtable(struct gmtp_hashtable *htable,
		unsigned int size, struct gmtp_hash_ops hash_ops)
{
	int i;

	gmtp_print_function();
	gmtp_print_debug("Size of gmtp_hashtable: %d", size);

	if(size < 1)
		return -ENOBUFS;

	if(htable == NULL)
		return -ENOBUFS;

	htable->entry = kmalloc(sizeof(void*) * size, GFP_KERNEL);

	if(htable->entry == NULL)
		return NULL;

	for(i = 0; i < size; ++i)
		htable->entry[i] = NULL;

	htable->size = size;
	htable->len = 0;
	htable->hash_ops = hash_ops;
	htable->hashval = hash_ops.hashval;
	htable->add_entry = hash_ops.add_entry;
	htable->del_entry = hash_ops.del_entry;
	htable->destroy = hash_ops.destroy;

	return 0;
}
EXPORT_SYMBOL_GPL(gmtp_build_hashtable);

void kfree_gmtp_hashtable(struct gmtp_hashtable *table)
{
	table->destroy(table);
}
EXPORT_SYMBOL_GPL(kfree_gmtp_hashtable);

unsigned int gmtp_hashval(struct gmtp_hashtable *table, const __u8 *key)
{
	unsigned int hashval;
	int i;

	if(unlikely(table == NULL))
		return -EINVAL;

	if(unlikely(key == NULL))
		return -ENOKEY;

	hashval = 0;
	for(i = 0; i < GMTP_HASH_KEY_LEN; ++i)
		hashval = key[i] + (hashval << 5) - hashval;

	return hashval % table->size;
}

struct gmtp_hash_entry *gmtp_lookup_entry(struct gmtp_hashtable *table,
		const __u8 *key)
{
	struct gmtp_hash_entry *entry;
	unsigned int hashval = table->hashval(table, key);

	/* Error */
	if(hashval < 0)
		return NULL;

	entry = table->entry[hashval];
	for(; entry != NULL; entry = entry->next)
		if(memcmp(key, entry->key, GMTP_HASH_KEY_LEN) == 0)
			return entry;
	return NULL;
}

int gmtp_add_entry(struct gmtp_hashtable *table, struct gmtp_hash_entry *entry)
{
	struct gmtp_hash_entry *cur_entry;
	unsigned int hashval;

	gmtp_print_function();

	hashval = table->hashval(table, entry->key);
	if(hashval < 0)
		return hashval;

	/** Primary key at client hashtable is flowname */
	cur_entry = gmtp_lookup_entry(table, entry->key);
	if(cur_entry != NULL)
		return 1; /* Entry already exists */

	entry->next = table->entry[hashval];
	table->entry[hashval] = entry;
	table->len++;

	return 0;
}

void destroy_gmtp_hashtable(struct gmtp_hashtable *table)
{
	int i;
	struct gmtp_hash_entry *entry, *tmp;

	gmtp_print_function();

	if(table == NULL)
		return;

	for(i = 0; i < table->size; ++i) {
		entry = table->entry[i];
		while(entry != NULL) {
			tmp = entry;
			entry = entry->next;
			table->del_entry(table, tmp->key);
		}
	}

	kfree(table->entry);
}



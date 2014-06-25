/**
 * @file lsslot.c
 *
 * Copyriht (C) 2005 IBM Corporatio
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "rtas_calls.h"
#include "drpci.h"
#include "dr.h"
#include "lsslot.h"
#include "drmem.h"
#include "pseries_platform.h"

int output_level = 0;
int log_fd = 0;

/**
 * struct print_node
 * @ brief struct to track list of nodes to be printed.
 */
struct print_node {
	struct dr_node	*node;	/**< node information */
	char		*desc;	/**< message description from catalog*/
	struct print_node	*next;
};

struct print_node *print_list = NULL;

/* These are used to determine column widths for output */
uint32_t max_sname = 0;		/* Max size of node location codes */
uint32_t max_desc = 0;		/* Max size of node descriptions */
#define LNAME_SIZE 12           /* xxxx:xx:xx.x */

/**
 * usage
 * @brief Print the usage message and exit
 */
static void
usage(void)
{
	fprintf(stderr, "Usage: lsslot [-c | -a | -b | -p | -o | -s ]"
		"[-F | -d | -w]\n");
	fprintf(stderr, "        -c <connector type>\n");
	fprintf(stderr, "                Display the slots of the specified "
		"connector type.  The valid\n");
	fprintf(stderr, "                connector types are \"pci\" for "
		"hotplug PCI slots, \"slot\" for\n");
	fprintf(stderr, "                logical slots, \"phb\" for PHB's, "
		"\"port\" for LHEA ports, \"mem\"\n");
	fprintf(stderr, "                for memory, and \"cpu\" "
		"for cpu's. The default\n");
	fprintf(stderr, "                is \"slot\" if no -c option is "
		"specified.\n");
	fprintf(stderr, "        -a      Display available slots, valid for "
		"\"pci\" slots only.\n");
	fprintf(stderr, "        -b      Display cpu's and caches, valid for "
		"\"cpu\" only.\n");
	fprintf(stderr, "        -o      Display occupied slots, valid for "
		"\"pci\" slots only.\n");
	fprintf(stderr, "        -p      Display caches, valid for \"cpu\" "
		"slots only.\n");
	fprintf(stderr, "        -s <slot>\n");
	fprintf(stderr, "                Display characteristics of the "
		"specified slot.\n");
	fprintf(stderr, "        -F <delimiter>\n");
	fprintf(stderr, "                Specified a single character to "
		"delimit the output.  The \n");
	fprintf(stderr, "                heading is not displayed and the "
		"columns are delimited by the\n");
	fprintf(stderr, "                specified character.\n");
	fprintf(stderr, "        -d      Enable debugging output.\n");
	fprintf(stderr, "        -w <timeout>\n");
	fprintf(stderr, "                Specify a timeout when attempting to "
		"acquire locks.\n");

	exit (1);
}

/**
 * free_print_nodes
 * @brief Free all of the items on the print_list
 *
 * NOTE: We do not free the node pointed to by the print_node, these
 * nodes exist on other lists and should be free when those lists are
 * cleaned up.
 */
static void
free_print_list(void)
{
	struct print_node *pnode;

	while (print_list != NULL) {
		pnode = print_list;
		print_list = print_list->next;
		free(pnode);
	}
}

/**
 * loc_code_cmp
 *
 * loc_code_cmp is used to sort a list of nodes based on their location code.
 * location codes take the form of
 *
 *      pn[.n][- or /]pn[.n][- or /] ...
 *
 * where p is an alpha location type prefix and n is an instance
 * number (see RS/6000 Processor Architecture, Location Code Format).
 * The location code has to be parsed by hyphens, if any.
 *
 * @param node1
 * @param node2
 * @returns 0 if (node1 = node2), -1 if (node1 < node2), 1 if (node1 > node2)
 */
static int
loc_code_cmp(char *node1, char *node2)
{
	char save_n1[64],save_n2[64];	/* for holding node1 and node2       */
	char *n1, *n2;
	ulong nbr1,nbr2;		/* hex numbers representing location */
	size_t dash_cnt;

	n1 = save_n1;
	n2 = save_n2;

	while (strlen(node1) && strlen(node2)) {
		dash_cnt = strcspn(node1, "-");
		strncpy(n1, node1, dash_cnt);

		*(n1 + dash_cnt) = '\0';
		node1 += dash_cnt;

		dash_cnt = strcspn(node2, "-");
		strncpy(n2, node2, dash_cnt);

		*(n2 + dash_cnt) = '\0';
		node2 += dash_cnt;

		/* First look at the location type */
		if (*n1 < *n2)
			return -1;
		else if (*n1 > *n2)
			return 1;

		n1++;
		n2++;

		/* get the hex value of the instance number */
		nbr1 = strtoul(n1, &n1, 16);
		nbr2 = strtoul(n2, &n2, 16);

		if (nbr1 < nbr2)
			return -1;
		else if (nbr1 > nbr2)
			return 1;

		if (strlen(n1) && strlen(n2)) {
			/* If both strings have more characters, first
			 * determine if they are the same.
			 */
			if (*n1 == *n2) {
				/* If they're the same, compare whatever
				 * follows the delimiter. The slash will have
				 * an alpha and hex following it, while the dot
				 * will have just a hex following it.
				 */
				if (*n1 == '/') {
					n1++;
					n2++;
					/* First look at the location type,
					 * which is a single character.
					 */
					if (*n1 < *n2)
					  	return -1;
					else if (*n1 > *n2)
					   	return 1;
				}

				n1++;
				n2++;

				/* get the hex value of the instance number */
				nbr1 = strtoul(n1, &n1, 16);
				nbr2 = strtoul(n2, &n2, 16);
				if (nbr1 < nbr2)
					return -1;
				else if (nbr1 > nbr2)
				   	return 1;
			}

			/* The delimiters are not the same, so check
			 * what they are and return results based on
			 * order of precedence : slash, then dot.
			 */
			else if (*n1 == '/')
				return -1;
			else if (*n2 == '/')
				return 1;
		}

		/* If we've reached here, either the strings are
		 * the same or one of the strings has run out.
		 * If only one has run out, we can return.
		 */
		if (strlen(n1) < strlen(n2))
			return -1;
		else if (strlen(n1) > strlen(n2))
			return 1;

		/* If we get here, we've determined everything
		 * in the n1 and n2 strings are the same. Now
		 * increment past the dash, if any, in the
		 * original strings.
		 */
		if (*node1 == '-')
			node1++;
		if (*node2 == '-')
			node2++;

	}

	if (strlen(node1))
		return 1;
	else if (strlen(node2))
		return -1;

	return 0;
}


/**
 * insert_print_node
 *
 * Insert the node into the list of nodes. The list is
 * sorted by location codes.
 *
 * @param node dlpar node to add
 */
void
insert_print_node(struct dr_node *node)
{
	struct print_node *pnode;

	pnode = zalloc(sizeof(*pnode));
	if (pnode == NULL) {
		fprintf(stderr, "Could not allocate print node for drc %x\n",
			node->drc_index);
		return;
	}

	pnode->node = node;
	pnode->desc = node_type(node);
	pnode->next = NULL;

	max_sname = MAX(max_sname, strlen(node->drc_name));
	max_desc = MAX(max_desc, strlen(pnode->desc));

	/* Insert the new print_node into the sorted list of print nodes */
	if (print_list == NULL) {
		print_list = pnode;
		return;
	}
	
	if (loc_code_cmp(print_list->node->drc_name,
			 pnode->node->drc_name) > 0) {
		/* The location code for the new node is less than that
		 * of the first node so insert the new node at the front.
		 */
		pnode->next = print_list;
		print_list = pnode;
	} else {
		/* Find the first node in the list where the new node's
		 * location code is less than the existing node's location
		 * code and insert the new node before that node.
		 */
		struct print_node *last;
		last = print_list;
		while (last->next != NULL) {
			if (loc_code_cmp(last->next->node->drc_name,
					 pnode->node->drc_name) > 0)
			{
				pnode->next = last->next;
				last->next = pnode;
				break;
			}
			last = last->next;
		}

		/* Put the new node at the end of the list if itslocation
		 * code is not less than any other node's location code.
		 */
		if (last->next == NULL)
			last->next = pnode;
	}
}

/**
 * print_drslot_line
 * @brief print a SLOT entry
 * 
 * @param pnode print_node to print
 * @param fmt output format string
 */
static void
print_drslot_line(struct print_node *pnode, char *fmt)
{
	struct dr_node *node = pnode->node;
	char *linux_dname;

	/* Print node name, description, and linux name */
	if (node->sysfs_dev_path[0])
		linux_dname = strrchr(node->sysfs_dev_path, '/') + 1;
	else
		linux_dname = "?";

	printf(fmt, node->drc_name, pnode->desc, linux_dname);

	/* If no node info, then it is an empty node */
	if (node->dev_type == HEA_DEV) {
		struct dr_node *port = node->children;
		
		if (!port) {
			printf("Empty\n");
		} else {
			int first = 1;
			for (port = node->children; port; port = port->next) {
				printf("%s%s ", first ? "" : ",",
					port->drc_name);
				first = 0;
			}
			printf("\n");
		}
	} else {
		if (node->ofdt_dname[0] == '\0')
			printf("Empty\n");
		else
			printf("%s\n", node->ofdt_dname);
	}
}

/**
 * print_phpslot_line
 * @brief print a hotplug slot entry
 *
 * @param pnode print_node to print
 * @param fmt output format string
 */
static void
print_phpslot_line(struct print_node *pnode, char *fmt)
{
	struct dr_node *child;
	struct dr_node *node = pnode->node;

	/* Print node name and description */
	printf(fmt, node->drc_name, pnode->desc);

	/* If no node info, then it is an empty node */
	if (! node->children)
		printf("Empty\n");
	else {
		/* Else we want to print the device names */
		for (child = node->children; child; child = child->next) {
			if (child != node->children)
				printf(fmt, "", "");

			if (child->sysfs_dev_path[0])
				printf("%s\n", strrchr(child->sysfs_dev_path, '/') + 1);
			else if (child->ofdt_dname[0])
				printf("%s\n", child->ofdt_dname);
			else
				printf("?\n");
		}
	}
}

/**
 * parse_options
 * @brief parse the command line options and fillin the cmd_opts struct
 *
 * @param argc
 * @param argv
 * @param opts
 */
static void
parse_options(int argc, char *argv[], struct cmd_opts *opts)
{
	int c;

	while ((c = getopt(argc, argv, "abc:d:F:ops:w:")) != EOF) {
		switch (c) {
		    case 'a':
			opts->a_flag = 1;
			break;

		    case 'b':
			opts->b_flag = 1;
			break;

		    case 'c':
			if (! strcmp(optarg, "phb"))
				opts->slot_type = PHB;
			else if (! strcmp(optarg, "slot"))
				opts->slot_type = SLOT;
			else if (! strcmp(optarg, "pci"))
				opts->slot_type = PCI;
			else if (! strcmp(optarg, "cpu"))
				opts->slot_type = CPU;
			else if (! strcmp(optarg, "mem"))
				opts->slot_type = MEM;
			else if (! strcmp(optarg, "port"))
				opts->slot_type = PORT;
			else {
				printf("\nThe specified connector type "
				       "is invalid.\n\n");
				usage();
			}
			break;

		    case 'd':
			set_output_level(atoi(optarg));
			break;

		    case 'F':
			opts->delim = optarg;
			/* make sure the arg specified is only one character
			 * long and is not the '%' character which would
			 * confuse the formatting.
			 */
			if ((opts->delim[1] != '\0')
			    || (opts->delim[0] == '%')) {
				say(ERROR, "You may specify only one character "
				    "for the -F option,\nand it must not "
				    "be the %% character.\n");
				exit(1);
			}
			break;

		    case 'o':
			opts->o_flag = 1;
			break;

		    case 'p':
			opts->p_flag = 1;
			break;

		    case 's':
			opts->s_name = optarg;
			break;

		    case 'w':
			opts->timeout = strtoul(optarg, NULL, 10) * 60;
			if (opts->timeout < 0)
				usage();
			break;

		    default:
			usage();
			break;
		}
	}

	/* Validate the options */
	switch (opts->slot_type) {
	    case SLOT:
	    case PORT:
		/* The a,b,o,p flags are not valid for slot */
		if (opts->a_flag || opts->b_flag || opts->o_flag ||
		    opts->p_flag)
			usage();

		/* Now, to make the code work right (which is wrong) we
		 * need to set the a and o flags if the s flag wasn't
		 * specified.
		 */
		if (opts->s_name == NULL) {
			opts->a_flag = 1;
			opts->o_flag = 1;
		}

		break;

	    case PHB:
		/* The a,b,F,o,p options are not valid for phb */
		if (opts->a_flag || opts->b_flag || opts->delim ||
		    opts->o_flag || opts->p_flag)
			usage();
		break;

	    case PCI:
		/* The b,p flags are valid for pci */
		if (opts->b_flag || opts->p_flag)
			usage();

		/* If no flags specified, then set a_flag and o_flag
		 * so that all slots will be formatted in the output
		 */
		if ((! opts->a_flag) && (! opts->o_flag)
		    && (opts->s_name == NULL)) {
			opts->a_flag = 1;
			opts->o_flag = 1;
		}

		break;

	    case CPU:
		/* The a,F,o,s options are not valid for cpu */
		if (opts->a_flag || opts->delim || opts->o_flag ||
		    opts->s_name)
			usage();

		if (opts->b_flag && opts->p_flag) {
			say(ERROR, "You cannot specify both the -b and -p "
			    "options.\n");
			usage();
		}

		break;
	}
}

/**
 * lsslot_chrp_pci
 * @brief main entry point for lsslot command
 *
 * @param opts
 * @returns 0 on success, !0 otherwise
 */
int
lsslot_chrp_pci(struct cmd_opts *opts)
{
	struct dr_node *all_nodes;	/* Pointer to list of all node info */
	struct dr_node *node;	/* Used to traverse list of node info */
	char	fmt[128];
	struct print_node *p;
	char *sheading = "# Slot";	/* Used in printing headers */
	char *dheading = "Description";	/* Used in printing headers */
	char *lheading = "Device(s)";	/* Used in printing headers */
	char *lname_header = "Linux Name";
	int rc = 0;

	/* Set initial column sizes */
	max_sname = MAX(max_sname, strlen(sheading));
	max_desc = MAX(max_desc, strlen(dheading));

	/* Get all of node(logical DR or PCI) node information */
	if (opts->slot_type == PCI)
		all_nodes = get_hp_nodes();
	else
		all_nodes = get_dlpar_nodes(PCI_NODES | VIO_NODES | HEA_NODES);

	/* If nothing returned, then no hot plug node */
	if (all_nodes == NULL) {
		if (opts->slot_type == PCI)
			say(ERROR, "There are no PCI hot plug slots on "
			    "this system.\n");
		else
			say(ERROR, "There are no DR slots on this system.\n");
   		return 0;
	}

	print_node_list(all_nodes);

	/* Otherwise, run through the node list looking for the nodes
	 * we want to print
	 */
	for (node = all_nodes; node; node = node->next) {
		if (! node->is_owned || node->skip)
			continue;
		
		if (opts->s_name != NULL) {
			if (cmp_drcname(node->drc_name, opts->s_name))
				insert_print_node(node);
		}

		/* If aflag and slot is empty, then format the slot */
		else if (opts->a_flag && (node->children == NULL))
			insert_print_node(node);

		/* If oflag and slot occupied, then format the slot */
		else if (opts->o_flag && (node->children != NULL))
			insert_print_node(node);
	}

	if (print_list == NULL) {
		/* If nothing to print, display message based on if
		 * user specified a slot or a device name.
		 */
		if (opts->s_name != NULL) {
			say(ERROR, "The specified PCI slot is either invalid\n"
			    "or does not support hot plug operations.\n");
			rc = 1;
		}
		goto lsslot_pci_exit;
	}

	/* This creates a format string so that slot name and description
	 * prints out in the required field width. When the -F flag is
	 * specified, the format string contains the delimiting character
	 * which the user specified at the command line.
	 */
	if (opts->slot_type == SLOT) {
		if (opts->delim != NULL)
			sprintf(fmt, "%s%s%s%s%s%s", "%s", opts->delim,
				"%s", opts->delim, "%s", opts->delim);
		else {
			sprintf(fmt, "%%-%ds%%-%ds%%-%ds", max_sname + 2,
				max_desc + 2, LNAME_SIZE + 2);
			/* Print out the header. */
			printf(fmt, sheading, dheading, lname_header);
			printf("%s\n", lheading);
		}
	} else {
		if (opts->delim != NULL)
			sprintf(fmt, "%s%s%s%s", "%s", opts->delim,
				"%s", opts->delim);
		else {
			sprintf(fmt, "%%-%ds%%-%ds", max_sname + 2,
				max_desc + 2);
			/* Print out the header. */
			printf(fmt, sheading, dheading);
			printf("%s\n", lheading);
		}
	}

	/* Now run through the list of slots we actually want to print */
	for (p = print_list; p != NULL; p = p->next) {
		if (! p->node->is_owned) {
			/* skip it, because the partition doesn't own it */
			continue;
		}

		if (opts->slot_type == SLOT)
			print_drslot_line(p, fmt);
		else
			print_phpslot_line(p, fmt);
	}

lsslot_pci_exit:
	free_print_list();
	free_node(all_nodes);
	return rc;
}

/**
 * lsslot_chrp_phb
 * @brief Main entry point for handling lsslot_chrp_phb command
 *
 * @param opts pointer to cmd_opts struct
 * @returns 0 on success, !0 otherwise
 */
int
lsslot_chrp_phb(struct cmd_opts *opts)
{
	struct dr_node *phb_list;
	struct dr_node *phb;

	phb_list = get_dlpar_nodes(PHB_NODES);
	if (phb_list == NULL)
		return -1;

	/* display header */
	printf("%-10s%-20s %s\n", "PHB name", "OFDT Name", "Slot(s) Connected");

	for (phb = phb_list; phb; phb = phb->next) {
		struct dr_node *child;
		char *name;
		int printed_count = 0;
		
		if ((opts->s_name != NULL) && 
		    (strcmp(opts->s_name, phb->drc_name)))
			continue;

		name = strstr(phb->ofdt_path, "/pci");
		printf("%-10s%-20s ", phb->drc_name, name);

		for (child = phb->children; child; child = child->next) {
			if (! child->is_owned)
				continue;

			if (printed_count == 0)
				printf("%s\n", child->drc_name);
			else
				printf("%-30s %s\n", "", child->drc_name);

			printed_count++;
		}

		if (printed_count)
			printf("\n");
		else
			printf("\n\n");
	}

	return 0;
}

int
lsslot_chrp_mem(void)
{
	struct lmb_list_head *lmb_list;
	struct dr_node *lmb;
	struct mem_scn *scn;
	int scn_offset = strlen("/sys/devices/system/memory/memory");
	int lmb_offset = strlen(OFDT_BASE);

	lmb_list = get_lmbs(LMB_NORMAL_SORT);
	if (lmb_list == NULL || lmb_list->lmbs == NULL)
		return -1;

	printf("lmb size: 0x%x\n", lmb_list->lmbs->lmb_size);
	
	if (lmb_list->drconf_buf) {
		printf("ibm,dynamic-reconfiguration-memory\n");
		printf("%-5s  %c  %s\n", "Name", 'R', "Sections");
		printf("%-5s  %c  %s\n", "----", '-', "--------");
	} else {
		printf("%-20s  %-5s  %c  %s\n", "Memory Node", "Name", 'R',
		       "Sections");
		printf("%-20s  %-5s  %c  %s\n", "-----------", "----", '-',
		       "--------");
	}

	for (lmb = lmb_list->lmbs; lmb; lmb = lmb->next) {
		int first = 1;

		if (!lmb->is_owned)
			continue;

		if (!lmb_list->drconf_buf)
			printf("%-20s  ", &lmb->ofdt_path[lmb_offset]);

		printf("%-5s  %c ", lmb->drc_name,
		       lmb->is_removable ? 'Y' : 'N');
		
		for (scn = lmb->lmb_mem_scns; scn; scn = scn->next) {
			if (first) {
				printf(" %s", &scn->sysfs_path[scn_offset]);
				first = 0;
			} else
				printf(", %s", &scn->sysfs_path[scn_offset]);
		}

		printf("\n");
	}

	free_lmbs(lmb_list);

	return 0;
}

/**
 * lsslot_chrp_port
 * @brief Print LHEA ports based on command line options
 *
 * @param opts
 * @returns 0 on success, !0 otherwise
 */
int
lsslot_chrp_port(struct cmd_opts *opts)
{
	struct dr_node *all_nodes;	/* Pointer to list of all node info */
	struct dr_node *node;		/* Used to traverse list of node info */
	struct dr_node *child;          /* Used to traverse list of children */
	char	fmt[128];
	struct print_node *p;
	char *sheading = "LHEA port name";	/* Used in printing headers */
	char *dheading = "Description";		/* Used in printing headers */
	int rc = 0;

	/* Set initial column sizes */
	max_sname = MAX(max_sname, strlen(sheading));
	max_desc = MAX(max_desc, strlen(dheading));

	all_nodes = get_dlpar_nodes(HEA_NODES);

	/* If nothing returned, then no hot plug node */
	if (all_nodes == NULL) {
		say(ERROR, "There are no LHEA ports on this system.\n");
		return 1;
	}

	print_node_list(all_nodes);

	/* Otherwise, run through the node list looking for the nodes
	 * we want to print
	 */
	for (node = all_nodes; node; node = node->next) {
		if (node->skip)
			continue;

		for (child = node->children; child; child = child->next) {
			if (child->skip)
				continue;
			/* If there is a search parameter, add matching ports.
			 * If there is no search, add all the ports.
			 */
			if (opts->s_name != NULL) {
				if (cmp_drcname(child->drc_name, opts->s_name))
					insert_print_node(child);
			} else
				insert_print_node(child);
		}
	}

	if (print_list == NULL) {
		/* If nothing to print, display message based on if
		 * user specified a slot or a device name.
		 */
		if (opts->s_name != NULL) {
			say(ERROR, "The specified port was not found.\n");
			rc = 1;
		}
		goto lsslot_port_exit;
	}

	/* This creates a format string so that port name and description
	 * prints out in the required field width. When the -F flag is
	 * specified, the format string contains the delimiting character
	 * which the user specified at the command line.
	 */
	if (opts->delim != NULL)
		sprintf(fmt, "%s%s%s\n", "%s", opts->delim, "%s");
	else {
		sprintf(fmt, "%%-%ds%%-%ds\n", max_sname + 2, max_desc + 2);
		/* Print out the header. */
		printf(fmt, sheading, dheading);
	}

	/* Now run through the list of ports we actually want to print */
	for (p = print_list; p != NULL; p = p->next) {
		printf(fmt, p->node->drc_name, p->desc);
	}

lsslot_port_exit:
	free_print_list();
	free_node(all_nodes);
	return rc;
}

int
main(int argc, char *argv[])
{
	struct cmd_opts opts;
	int rc;

	switch (get_platform()) {
	case PLATFORM_UNKNOWN:
	case PLATFORM_POWERKVM_HOST:
		fprintf(stderr, "%s: is not supported on the %s platform\n",
						argv[0], platform_name);
		exit(1);
	}

	/* make sure that we're running on the proper platform.	*/
	if (! valid_platform("chrp"))
		exit(1);

	memset(&opts, 0, sizeof(opts));

	/* default to DRSLOT type */
	opts.slot_type = SLOT;
	parse_options(argc, argv, &opts);

	rc = dr_lock();
	if (rc) {
		say(ERROR, "Unable to obtain Dynamic Reconfiguration lock. "
		    "Please try command again later.\n");
		exit(1);
	}

	switch (opts.slot_type) {
	    case SLOT:
	    case PCI:
		rc = lsslot_chrp_pci(&opts);
		break;

	    case PHB:
		rc = lsslot_chrp_phb(&opts);
		break;

	    case CPU:
		rc = lsslot_chrp_cpu(&opts);
		break;

	    case MEM:
		rc = lsslot_chrp_mem();
		break;

	    case PORT:
		rc = lsslot_chrp_port(&opts);
		break;
	}

	dr_unlock();
	exit(rc);
}

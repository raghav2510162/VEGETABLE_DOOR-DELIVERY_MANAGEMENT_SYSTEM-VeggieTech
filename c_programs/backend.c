/**
 * backend.c - Unified Stateful Backend for VeggieTech
 * =====================================================
 * This is the single C process that runs alongside Flask. It:
 *  1. Loads all data from text files once at startup (load_data).
 *  2. Keeps all data in memory (structs / linked lists).
 *  3. Processes commands from Flask via stdin in a loop.
 *  4. Writes all data back to text files only when it receives SHUTDOWN.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Configuration ─────────────────────────────────────── */
#define DATA_DIR "data/"
#define INVENTORY_FILE DATA_DIR "inventory.txt"
#define ORDERS_FILE DATA_DIR "orders.txt"
#define PAYMENTS_FILE DATA_DIR "payments.txt"
#define ADMINS_FILE DATA_DIR "admins.txt"

#define MAX_VEGS 50
#define MAX_LINE 4096
#define MAX_ITEMS_STR 2048
#define INV_HT_SIZE 64

/* ── Data Structures ────────────────────────────────────── */
typedef struct { /*Array Node for Inventory*/
  char name[50];
  int price;
  double stock;
  char unit[20];
} Item;

/* 1b. Inventory Hash Table Entry */
typedef struct InvHTEntry {
  char key[50];
  Item *item; /* Pointer to the real Item in the array */
  struct InvHTEntry *next;
} InvHTEntry;

/* Forward declaration for OrderNode so PendingQNode can use it */
struct PendingQNode;

/* 2. Order Node (DLL) */
typedef struct OrderNode { /*Queue Node for Orders*/
  char id[30];
  char name[100];
  char phone[20];
  char email[100];
  char address[200];
  char payment[20];
  char items_str[MAX_ITEMS_STR];
  double subtotal;
  double total;
  char status[30];
  char date[20];

  struct PendingQNode *queue_location; /* O(1) Queue Removal */

  struct OrderNode *next;
  struct OrderNode *prev; /* DLL pointer */
} Order;

/* 3. Pending Queue Node (DLL) */
typedef struct PendingQNode { /*Queue Node for Pending Orders*/
  Order *order;               /* Pointer back to the main Order */
  struct PendingQNode *next;
  struct PendingQNode *prev;
} PendingQNode;

/* 4. Payment Node (SLL) */
typedef struct PayNode { /*Linked List for Payments*/
  char order_id[30];
  char phone[20];
  char method[20];
  char status[10];
  struct PayNode *next;
} Payment;

/* ── Global In-Memory State ─────────────────────────────── */
static Item g_inventory[MAX_VEGS];
static int g_inv_count = 0;
static InvHTEntry *g_inv_ht[INV_HT_SIZE] = {NULL};

static Order *g_orders_head = NULL;
static Order *g_orders_tail = NULL; /*Makes insertion efficient O(1)*/

static Payment *g_payments_head = NULL;
static Payment *g_payments_tail = NULL; /* SLL Tail for FIFO */

static PendingQNode *g_pending_head = NULL;
static PendingQNode *g_pending_tail = NULL;

static char g_admin_user[100] = "";
static char g_admin_pass[100] = "";
static int g_order_counter =
    0; /* Tracks total orders ever to prevent ID collision */

/* ── Utility & Helpers ──────────────────────────────────── */

/* Strip trailing \r\n from a string */
static void strip_nl(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
}

/* Basic email validation: simple check for @ and a dot following it */
static int is_valid_email(const char *email) {
  const char *at = strchr(email, '@'); /*pointer to first occurrence of @*/
  if (!at || at == email ||
      !*(at + 1)) /*email contains '@', '@' is not the first character,
                     characters exist after '@'*/
    return 0;
  const char *dot = strrchr(email, '.'); /*pointer to last occurrence of .*/
  if (!dot || dot < at + 2 ||
      !*(dot + 1)) /*email contains '.', '.' exists after '@', characters exist
                      after '.'*/
    return 0;
  return 1;
}

/* Case-insensitive strcmp */
static int ci_cmp(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a; /*convert to lowercase*/
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b; /*convert to lowercase*/
    if (ca != cb) {
      return ca - cb;
    }
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

/* Splits a line by delimiter into parts array. Returns count. */
static int split(char *line, char delim, char **parts, int max) {
  int n = 0;
  if (!line || !*line) {
    return 0;
  }
  parts[n++] = line;
  while (*line && n < max) {
    if (*line == delim) {
      *line = '\0'; /*ends the current part(C stops reading after /0)*/
      parts[n++] =
          line + 1; /*moves pointer to the next position to start reading*/
    }
    line++;
  }
  return n;
}

/* ── Hash Table Operations (O(1)) ───────────────────────── */
static unsigned int ht_hash(const char *s) {
  unsigned int h = 5381;
  while (*s)
    h = ((h << 5) + h) + (unsigned char)*s++;
  return h % INV_HT_SIZE;
}

static void inv_ht_insert(Item *it) {
  unsigned int idx = ht_hash(it->name);
  InvHTEntry *e = calloc(1, sizeof(InvHTEntry));
  strcpy(e->key, it->name);
  e->item = it;
  e->next = g_inv_ht[idx];
  g_inv_ht[idx] = e;
}

static Item *inv_ht_find(const char *name) {
  unsigned int idx = ht_hash(name);
  for (InvHTEntry *e = g_inv_ht[idx]; e; e = e->next) {
    if (strcmp(e->key, name) == 0)
      return e->item;
  }
  return NULL;
}

/* ── DLL Pending Queue Operations ───────────────────────── */
/* O(1) Rear Enqueue */
static void enqueue_pending(Order *o) {
  PendingQNode *n = calloc(1, sizeof(PendingQNode));
  n->order = o;
  n->next = NULL;
  n->prev = g_pending_tail;

  if (!g_pending_head) {
    g_pending_head = g_pending_tail = n;
  } else {
    g_pending_tail->next = n;
    g_pending_tail = n;
  }
  o->queue_location = n; /* Link back to order for O(1) removal */
}

/* O(1) Direct Removal (No Traversal) */
static void remove_from_pending_direct(Order *o) {
  PendingQNode *qnode = o->queue_location;
  if (!qnode)
    return; /* Not in queue */

  if (qnode->prev)
    qnode->prev->next = qnode->next;
  else
    g_pending_head = qnode->next;

  if (qnode->next)
    qnode->next->prev = qnode->prev;
  else
    g_pending_tail = qnode->prev;

  free(qnode);
  o->queue_location = NULL;
}

/* O(1) Front Dequeue for GET_NEXT_PENDING */
static Order *dequeue_pending() {
  if (!g_pending_head)
    return NULL;

  PendingQNode *qnode = g_pending_head;
  Order *o = qnode->order;

  g_pending_head = qnode->next;
  if (g_pending_head)
    g_pending_head->prev = NULL;
  else
    g_pending_tail = NULL;

  o->queue_location = NULL;
  free(qnode);
  return o;
}

/* ── Order DLL Operations ───────────────────────────────── */
/* Finds an order by ID (optionally checks phone as well) */
static Order *find_order(const char *id, const char *phone) {
  for (Order *o = g_orders_head; o; o = o->next) {
    if (strcmp(o->id, id) == 0) { /*if Order ID is matched*/
      if (!phone || strcmp(o->phone, phone) == 0) {
        return o;
      }
    }
  }
  return NULL;
}

/* Adds a new order to the DLL */
static void enqueue_order(Order *o) {
  o->next = NULL;
  o->prev = g_orders_tail;
  if (!g_orders_head) {
    g_orders_head = g_orders_tail = o;
  } else {
    g_orders_tail->next = o;
    g_orders_tail = o;
  }
}

/* ── Business Logic ─────────────────────────────────────── */
/* Processes the semi-colon-delimited item list */
static void process_items(const char *istr, int print, int restore) {
  char copy[MAX_ITEMS_STR], *parts[4], *items[64];
  strncpy(copy, istr, sizeof(copy) - 1); /*new local variable for iteration*/
  int n = split(copy, ';', items, 64);   /*split the item string by semicolon*/

  for (int i = 0; i < n; i++) {
    if (strlen(items[i]) <
        3) /*skips empty(errored) items like ";a;" where a proper string would
              look like "item:qty:unit:price"*/
      continue;
    if (split(items[i], ':', parts, 4) ==
        4) { /*checks if the item has 4 parts*/
      double qty = atof(parts[1]);
      int price = atoi(parts[3]);

      if (print) { /*check if print flag is set*/
        printf("ITEM:%s|%s|%s|%s|%.2f\n", parts[0], parts[1], parts[2],
               parts[3], qty * price);
      }
      if (restore) { /*check if restore flag is set*/
        /* O(1) Hash Table Lookup */
        Item *it = inv_ht_find(parts[0]);
        if (it) {
          it->stock += qty;
        }
      }
    }
  }
}

/* Prints a standardized SUCCESS block for an order */
static void print_order_success(Order *o) {
  printf(
      "SUCCESS\nORDER_ID:%s\nNAME:%s\nPHONE:%s\nEMAIL:%s\nADDRESS:%s\nPAYMENT:%"
      "s\nSUBTOTAL:%.2f\nDELIVERY:15.00\nTOTAL:%.2f\nSTATUS:%s\nDATE:%s\n",
      o->id, o->name, o->phone, o->email, o->address, o->payment, o->subtotal,
      o->total, o->status, o->date);
}

/* ── Load & Save Logic ──────────────────────────────────── */

static void load_data() {
  /* Admin Load */
  FILE *fa = fopen(ADMINS_FILE, "r");
  if (fa) {
    char l[256], *parts[2];
    if (fgets(l, 256, fa)) {
      strip_nl(l);
      if (split(l, ':', parts, 2) == 2) {
        strcpy(g_admin_user, parts[0]);
        strcpy(g_admin_pass, parts[1]);
      }
    }
    fclose(fa);
  } else {
    strcpy(g_admin_user, "admin");
    strcpy(g_admin_pass, "pass123");
  }

  /* Inventory Load */
  FILE *fi = fopen(INVENTORY_FILE, "r");
  if (fi) {
    char line[256], *parts[4];
    while (fgets(line, sizeof(line), fi) &&
           g_inv_count < MAX_VEGS) { /*line by line reading*/
      strip_nl(line);
      if (split(line, ':', parts, 4) == 4) {
        strncpy(g_inventory[g_inv_count].name, parts[0], 49);
        g_inventory[g_inv_count].price = atoi(parts[1]);
        g_inventory[g_inv_count].stock = atof(parts[2]);
        strncpy(g_inventory[g_inv_count].unit, parts[3], 19);

        /* Insert pointer to this array element into the hash table */
        inv_ht_insert(&g_inventory[g_inv_count]);
        g_inv_count++; /*array iterator*/
      }
    }
    fclose(fi);
  }

  /* Orders Load */
  FILE *fo = fopen(ORDERS_FILE, "r");
  if (fo) {
    char line[MAX_LINE], *p[11];
    while (fgets(line, sizeof(line), fo)) { /*line by line reading*/
      strip_nl(line);
      Order *o = calloc(1, sizeof(Order));
      int n = split(line, '|', p, 11); /*counts the number of parts*/
      if (n == 11) {
        strncpy(o->id, p[0], 29);
        strncpy(o->name, p[1], 99);
        strncpy(o->phone, p[2], 19);
        strncpy(o->email, p[3], 99);
        strncpy(o->address, p[4], 199);
        strncpy(o->payment, p[5], 19);
        strncpy(o->items_str, p[6], MAX_ITEMS_STR - 1);
        o->subtotal = atof(p[7]);
        o->total = atof(p[8]);
        strncpy(o->status, p[9], 29);
        strncpy(o->date, p[10], 19);

        /* Update ID counter based on existing IDs */
        int id_val = atoi(o->id + 4); /* Skip "#OD-" */
        if (id_val > g_order_counter)
          g_order_counter = id_val;

        enqueue_order(o); /*queue iterator*/

        if (strcmp(o->status, "Pending") == 0) {
          enqueue_pending(o);
        }
      } else {
        free(o);
      }
    }
    fclose(fo);
  }

  /* Payments Load */
  FILE *fp = fopen(PAYMENTS_FILE, "r");
  if (fp) {
    char l[512], *pt[4];
    while (fgets(l, 512, fp)) { /*line by line reading*/
      strip_nl(l);
      if (split(l, '|', pt, 4) == 4) {
        Payment *p = calloc(1, sizeof(Payment));
        strncpy(p->order_id, pt[0], 29);
        strncpy(p->phone, pt[1], 19);
        strncpy(p->method, pt[2], 19);
        strcpy(p->status, pt[3]);

        /* Tail insertion for payments (FIFO) */
        p->next = NULL;
        if (!g_payments_tail) {
          g_payments_head = g_payments_tail = p;
        } else {
          g_payments_tail->next = p;
          g_payments_tail = p;
        }
      }
    }
    fclose(fp);
  }
}

static void save_data() {
  FILE *fi = fopen(INVENTORY_FILE, "w");
  FILE *fo = fopen(ORDERS_FILE, "w");
  FILE *fp = fopen(PAYMENTS_FILE, "w");

  if (fi) {
    for (int i = 0; i < g_inv_count; i++) {
      fprintf(fi, "%s:%d:%.2f:%s\n", g_inventory[i].name, g_inventory[i].price,
              g_inventory[i].stock, g_inventory[i].unit);
    }
    fclose(fi);
  }

  if (fo) {
    for (Order *o = g_orders_head; o; o = o->next) {
      fprintf(fo, "%s|%s|%s|%s|%s|%s|%s|%.2f|%.2f|%s|%s\n", o->id, o->name,
              o->phone, o->email, o->address, o->payment, o->items_str,
              o->subtotal, o->total, o->status, o->date);
    }
    fclose(fo);
  }

  if (fp) {
    for (Payment *p = g_payments_head; p; p = p->next) {
      fprintf(fp, "%s|%s|%s|%s\n", p->order_id, p->phone, p->method, p->status);
    }
    fclose(fp);
  }
}

/* ── Command Handlers ───────────────────────────────────── */

static void cmd_place_order(char **args, int argc) {
  if (argc < 5 + g_inv_count) { /*check for if all user arguments are provided*/
    printf("ERROR:Missing args\nEND\n");
    return;
  }
  double sub = 0; /*subtotal*/
  char istr[MAX_ITEMS_STR] = "", date[20], oid[30];

  /* Positional lookup against the master array g_inventory */
  for (int i = 0; i < g_inv_count; i++) {
    double q = atof(args[5 + i]); /*args[6] to args[5 + g_inv_count] store qty
                                     of each item*/
    if (q <= 0)                   /*skips items whose quantity is zero */
      continue;

    if (q > g_inventory[i].stock) {
      printf("ERROR:Insufficient stock for %s\nEND\n", g_inventory[i].name);
      return;
    }
    sub += q * g_inventory[i].price;
    g_inventory[i].stock -= q;

    char b[128];
    snprintf(b, 128, "%s:%.2f:%s:%d;", g_inventory[i].name, q,
             g_inventory[i].unit,
             g_inventory[i].price); /*stores formated string*/
    strncat(istr, b, MAX_ITEMS_STR - strlen(istr) - 1);
  }

  if (sub < 100) {
    process_items(istr, 0, 1); /*restoring stock*/
    printf("ERROR:Min order Rs.100\nEND\n");
    return;
  }

  if (!is_valid_email(args[2])) {
    process_items(istr, 0, 1); /*restoring stock*/
    printf("ERROR:Invalid email format. Must contain @ and a valid domain "
           "(e.g. .com)\nEND\n");
    return;
  }

  g_order_counter++;
  time_t t = time(NULL);
  strftime(date, 20, "%d-%m-%Y", localtime(&t));
  snprintf(oid, 30, "#OD-%05d", g_order_counter);

  Order *o = calloc(1, sizeof(Order)); /*creates order*/
  strncpy(o->id, oid, 29);
  strncpy(o->name, args[0], 99);
  strncpy(o->phone, args[1], 19);
  strncpy(o->email, args[2], 99);
  strncpy(o->address, args[3], 199);
  strncpy(o->payment, args[4], 19);
  strncpy(o->items_str, istr, MAX_ITEMS_STR - 1);
  o->subtotal = sub;
  o->total = sub + 15;
  strcpy(o->status, "Pending");
  strcpy(o->date, date);

  enqueue_order(o);
  enqueue_pending(o);

  print_order_success(o);
  /* Clear stock visually in output */
  process_items(istr, 1, 0); /*removes stock*/
  printf("END\n");
}

static void cmd_update_status(char **args, int argc) {
  if (argc < 2) { /*check for if all arguments are provided*/
    printf("ERROR:Missing args\nEND\n");
    return;
  }
  Order *cur = g_orders_head;

  while (cur) {
    if (strcmp(cur->id, args[0]) == 0) {
      /* If status is set to 'Cancelled', we scratch the order off entirely */
      if (ci_cmp(args[1], "Cancelled") == 0) {
        /* O(1) Hash Table stock restore */
        process_items(cur->items_str, 0, 1); /*restoring stock*/

        /* O(1) Remove from Pending Queue */
        remove_from_pending_direct(cur);

        /* O(1) Remove from Order DLL */
        if (cur->prev)
          cur->prev->next =
              cur->next; /*middle or last node deletion(order is cancelled)*/
        else
          g_orders_head = cur->next; /*first node deletion(order is cancelled)*/

        if (cur->next)
          cur->next->prev = cur->prev;
        else
          g_orders_tail = cur->prev; /*updates tail if necessary*/

        /* Print details before freeing so Flask API can send an email */
        printf("SUCCESS\nMSG:Order %s has been deleted and stock "
               "restored.\nNAME:%s\nPHONE:%s\nEMAIL:%s\nADDRESS:%s\nTOTAL:%."
               "2f\nEND\n",
               args[0], cur->name, cur->phone, cur->email, cur->address,
               cur->total);
        free(cur);
        return;
      }

      /* Regular status update */
      strncpy(cur->status, args[1], 29);
      remove_from_pending_direct(cur); /* Safe to call even if not pending */

      printf("SUCCESS\nMSG:Updated to "
             "%s\nNAME:%s\nPHONE:%s\nEMAIL:%s\nADDRESS:%s\nTOTAL:%.2f\nEND\n",
             args[1], cur->name, cur->phone, cur->email, cur->address,
             cur->total);
      return;
    }
    cur = cur->next; /*queue traversal*/
  }
  printf("ERROR:Not found\nEND\n");
}

/* ── Main System ────────────────────────────────────────── */

static int tokenize(char *line, char **argv, int max) {
  int c = 0;
  char *p = line;
  while (*p && c < max) {
    while (*p == ' ' || *p == '\t')
      p++; /*only traverses characters, numbers and special characters(spaces
              are dealt with if conditions)*/
    if (!*p)
      break;         /*end of string*/
    if (*p == '"') { /*quote is encountered*/
      p++;
      argv[c++] = p;
      while (*p && *p != '"')
        p++;
      if (*p)
        *p++ =
            '\0'; /*saves string bw two "" and replaces with a null terminator*/
    } else {      /*space or tab is encountered*/
      argv[c++] = p;
      while (*p && *p != ' ' && *p != '\t')
        p++;
      if (*p)
        *p++ = '\0'; /*(*p) exists but is a space or tab*/
    }
  }
  return c;
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  signal(SIGINT, (void (*)(int))save_data); /*saves data on interrupt(Ctrl+C)*/

  load_data();

  char line[MAX_LINE], *argv[128];
  while (fgets(line, sizeof(line), stdin)) {
    strip_nl(line);
    int argc = tokenize(line, argv, 128);
    if (argc == 0)
      continue;

    char *cmd = argv[0];

    if (strcmp(cmd, "LOGIN") == 0) { /*action is LOGIN*/
      if (strcmp(argv[1], g_admin_user) == 0 &&
          strcmp(argv[2], g_admin_pass) ==
              0) { /*both username and password are correct*/
        printf("SUCCESS\nUSER:%s\nEND\n", g_admin_user);
      } else {
        printf("ERROR:Invalid\nEND\n");
      }
    } else if (strcmp(cmd, "GET_INVENTORY") == 0) { /*action is GET_INVENTORY*/
      printf("SUCCESS\nCOUNT:%d\n", g_inv_count);
      for (int i = 0; i < g_inv_count; i++) { /*iterative display*/
        printf("VEG:%s|%d|%.2f|%s\n", g_inventory[i].name, g_inventory[i].price,
               g_inventory[i].stock, g_inventory[i].unit);
      }
      printf("END\n");
    } else if (strcmp(cmd, "ADD_VEGETABLE") == 0) { /*action is ADD_VEGETABLE*/
      strncpy(g_inventory[g_inv_count].name, argv[1], 49);
      g_inventory[g_inv_count].price = atoi(argv[2]);
      g_inventory[g_inv_count].stock = atof(argv[3]);
      strncpy(g_inventory[g_inv_count].unit, argv[4], 19);
      inv_ht_insert(&g_inventory[g_inv_count]);
      g_inv_count++;
      printf("SUCCESS\nEND\n");
    } else if (strcmp(cmd, "UPDATE_INVENTORY") ==
               0) { /*action is UPDATE_INVENTORY*/
      for (int i = 0; i < g_inv_count; i++) {
        g_inventory[i].price = atoi(argv[1 + i * 2]);
        g_inventory[i].stock = atof(argv[2 + i * 2]);
      }
      printf("SUCCESS\nEND\n");
    } else if (strcmp(cmd, "PLACE_ORDER") == 0) { /*action is PLACE_ORDER*/
      cmd_place_order(argv + 1, argc - 1);        /*sends data to place order*/
    } else if (strcmp(cmd, "GET_ORDER") == 0) {   /*action is GET_ORDER*/
      Order *o = find_order(argv[1], argv[2]);    /*sends data to find order*/
      if (o) {
        print_order_success(o);
        process_items(o->items_str, 1, 0);
        printf("END\n");
      } else {
        printf("ERROR:Not found\nEND\n");
      }
    } else if (strcmp(cmd, "GET_ORDERS_ALL") ==
               0) { /*action is GET_ORDERS_ALL*/
      printf("SUCCESS\n");
      for (Order *o = g_orders_head; o; o = o->next) { /*iterative display*/
        printf("ORDER:%s|%s|%s|%s|%.2f|%s|%s\n", o->id, o->name, o->phone,
               o->payment, o->total, o->status, o->email);
      }
      printf("END\n");
    } else if (strcmp(cmd, "UPDATE_STATUS") == 0) { /*action is UPDATE_STATUS*/
      cmd_update_status(argv + 1, argc - 1);    /*sends data to update status*/
    } else if (strcmp(cmd, "PAY_ORDER") == 0) { /*action is PAY_ORDER*/
      Order *o = find_order(argv[1], argv[2]);  /*sends data to create payment*/
      if (o) {
        Payment *p = calloc(1, sizeof(Payment));
        strncpy(p->order_id, argv[1], 29);
        strncpy(p->phone, argv[2], 19);
        strncpy(p->method, argv[3], 19);
        strcpy(p->status, "Paid");
        p->next = NULL;
        if (!g_payments_tail) {
          g_payments_head = g_payments_tail = p;
        } else {
          g_payments_tail->next = p;
          g_payments_tail = p;
        }
        printf("SUCCESS\nNAME:%s\nTOTAL:%.2f\nEND\n", o->name, o->total);
      } else {
        printf("ERROR:Not found\nEND\n");
      }
    } else if (strcmp(cmd, "PAY_STATUS") == 0) { /*action is PAY_STATUS*/
      int found = 0;
      for (Payment *p = g_payments_head; p; p = p->next) {
        if (strcmp(p->order_id, argv[1]) == 0 &&
            strcmp(p->phone, argv[2]) ==
                0) { /*sends data to check payment status*/
          printf("STATUS:%s\nMETHOD:%s\nEND\n", p->status, p->method);
          found = 1;
          break;
        }
      }
      if (!found) {
        printf("STATUS:Unpaid\nEND\n");
      }
    } else if (strcmp(cmd, "GET_NEXT_PENDING") == 0) {
      Order *po = dequeue_pending();
      if (po) {
        print_order_success(po);
        printf("END\n");
      } else {
        printf("ERROR:No pending orders\nEND\n");
      }
    } else if (strcmp(cmd, "SHUTDOWN") == 0) { /*action is SHUTDOWN*/
      save_data();
      printf("SUCCESS\nEND\n");
      break;
    }
  }
  return 0;
}

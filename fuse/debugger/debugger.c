/* debugger.h: Fuse's monitor/debugger
   Copyright (c) 2002-2004 Philip Kendall

   $Id$

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

   Author contact information:

   E-mail: pak21-fuse@srcf.ucam.org
   Postal address: 15 Crescent Road, Wokingham, Berks, RG40 2DB, England

*/

#include <config.h>

#include "debugger.h"
#include "debugger_internals.h"
#include "event.h"
#include "memory.h"
#include "periph.h"
#include "ui/ui.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

/* The current activity state of the debugger */
enum debugger_mode_t debugger_mode;

/* The current breakpoints */
GSList *debugger_breakpoints;

/* The next breakpoint ID to use */
static size_t next_breakpoint_id;

/* Which base should we display things in */
int debugger_output_base;

static void remove_time( gpointer data, gpointer user_data );
static debugger_breakpoint* get_breakpoint_by_id( size_t id );
static gint find_breakpoint_by_id( gconstpointer data,
				   gconstpointer user_data );
static gint find_breakpoint_by_address( gconstpointer data,
					gconstpointer user_data );
static void free_breakpoint( gpointer data, gpointer user_data );
static void add_time_event( gpointer data, gpointer user_data );

/* Textual represenations of the breakpoint types and lifetimes */
const char *debugger_breakpoint_type_text[] = {
  "Execute", "Read", "Write", "Port Read", "Port Write", "Time",
};

const char *debugger_breakpoint_life_text[] = {
  "Permanent", "One Shot",
};

int
debugger_init( void )
{
  debugger_breakpoints = NULL;
  next_breakpoint_id = 1;
  debugger_output_base = 16;
  return debugger_reset();
}

int
debugger_reset( void )
{
  debugger_breakpoint_remove_all();
  debugger_mode = DEBUGGER_MODE_INACTIVE;

  return 0;
}

int
debugger_end( void )
{
  debugger_breakpoint_remove_all();
  return 0;
}

/* Check whether the debugger should become active at this point */
int
debugger_check( debugger_breakpoint_type type, libspectrum_dword value )
{
  GSList *ptr; debugger_breakpoint *bp;

  switch( debugger_mode ) {

  case DEBUGGER_MODE_INACTIVE: return 0;

  case DEBUGGER_MODE_ACTIVE:
    for( ptr = debugger_breakpoints; ptr; ptr = ptr->next ) {

      bp = ptr->data;

      if( bp->type != type ) continue;

      if( type == DEBUGGER_BREAKPOINT_TYPE_TIME ) {
	if( tstates < bp->value ) continue;
      } else {
	if( bp->page == -1 ) {
	  /* If the breakpoint doesn't have a page specified, the value
	     must match exactly */
	  if( bp->value != value ) continue;
	} else {
	  /* If it has a page specified, the page and offset must both
	     match */
	  if( bp->page != memory_map[ value >> 13 ].reverse ) continue;
	  if( bp->value != ( value & 0x3fff ) ) continue;
	}
      }

      if( bp->ignore ) { bp->ignore--; continue; }

      if( bp->condition && !debugger_expression_evaluate( bp->condition ) )
	continue;

      if( bp->life == DEBUGGER_BREAKPOINT_LIFE_ONESHOT ) {
	debugger_breakpoints = g_slist_remove( debugger_breakpoints, bp );
	free( bp );
      }

      debugger_mode = DEBUGGER_MODE_HALTED;
      return 1;
    }
    return 0;

  case DEBUGGER_MODE_HALTED: return 1;

  }
  return 0;	/* Keep gcc happy */
}

/* Activate the debugger */
int
debugger_trap( void )
{
  return ui_debugger_activate();
}

/* Step one instruction */
int
debugger_step( void )
{
  debugger_mode = DEBUGGER_MODE_HALTED;
  ui_debugger_deactivate( 0 );
  return 0;
}

/* Step to the next instruction, ignoring CALLs etc */
int
debugger_next( void )
{
  size_t length;

  /* Find out how long the current instruction is */
  debugger_disassemble( NULL, 0, &length, PC );

  /* And add a breakpoint after that */
  debugger_breakpoint_add( DEBUGGER_BREAKPOINT_TYPE_EXECUTE, -1,
			   PC + length, 0, DEBUGGER_BREAKPOINT_LIFE_ONESHOT,
			   NULL );

  debugger_run();

  return 0;
}

/* Set debugger_mode so that emulation will occur */
int
debugger_run( void )
{
  debugger_mode = debugger_breakpoints ?
                  DEBUGGER_MODE_ACTIVE :
                  DEBUGGER_MODE_INACTIVE;
  ui_debugger_deactivate( 1 );
  return 0;
}

/* Add a breakpoint */
int
debugger_breakpoint_add( debugger_breakpoint_type type, int page,
			 libspectrum_dword value, size_t ignore,
			 debugger_breakpoint_life life,
			 debugger_expression *condition )
{
  debugger_breakpoint *bp;

  bp = malloc( sizeof( debugger_breakpoint ) );
  if( !bp ) {
    ui_error( UI_ERROR_ERROR, "Out of memory at %s:%d", __FILE__, __LINE__ );
    return 1;
  }

  bp->id = next_breakpoint_id++; bp->type = type;
  bp->page = page;

  if( type == DEBUGGER_BREAKPOINT_TYPE_TIME ) {
    bp->value = value;
  } else {
    bp->value = (libspectrum_word)value;
  }

  bp->ignore = ignore; bp->life = life;
  bp->condition = condition;

  debugger_breakpoints = g_slist_append( debugger_breakpoints, bp );

  if( debugger_mode == DEBUGGER_MODE_INACTIVE )
    debugger_mode = DEBUGGER_MODE_ACTIVE;

  /* If this was a timed breakpoint, set an event to stop emulation
     at that point */
  if( type == DEBUGGER_BREAKPOINT_TYPE_TIME ) {
    int error;

    error = event_add( value, EVENT_TYPE_BREAKPOINT );
    if( error ) return error;
  }

  return 0;
}

struct remove_t {

  libspectrum_dword tstates;
  int done;

};

/* Remove breakpoint with the given ID */
int
debugger_breakpoint_remove( size_t id )
{
  debugger_breakpoint *bp;

  bp = get_breakpoint_by_id( id ); if( !bp ) return 1;

  debugger_breakpoints = g_slist_remove( debugger_breakpoints, bp );
  if( debugger_mode == DEBUGGER_MODE_ACTIVE && !debugger_breakpoints )
    debugger_mode = DEBUGGER_MODE_INACTIVE;

  /* If this was a timed breakpoint, remove the event as well */
  if( bp->type == DEBUGGER_BREAKPOINT_TYPE_TIME ) {

    struct remove_t remove;

    remove.tstates = bp->value;
    remove.done = 0;

    event_foreach( remove_time, &remove );
  }

  free( bp );

  return 0;
}

static void
remove_time( gpointer data, gpointer user_data )
{
  event_t *event;
  struct remove_t *remove;

  event = data; remove = user_data;

  if( remove->done ) return;

  if( event->type == EVENT_TYPE_BREAKPOINT &&
      event->tstates == remove->tstates ) {
    event->type = EVENT_TYPE_NULL;
    remove->done = 1;
  }
}

static debugger_breakpoint*
get_breakpoint_by_id( size_t id )
{
  GSList *ptr;

  ptr = g_slist_find_custom( debugger_breakpoints, &id,
			     find_breakpoint_by_id );
  if( !ptr ) {
    ui_error( UI_ERROR_ERROR, "Breakpoint %ld does not exist",
	      (unsigned long)id );
    return NULL;
  }

  return ptr->data;
}

static gint
find_breakpoint_by_id( gconstpointer data, gconstpointer user_data )
{
  const debugger_breakpoint *bp = data;
  size_t id = *(const size_t*)user_data;

  return bp->id - id;
}

/* Remove all breakpoints at the given address */
int
debugger_breakpoint_clear( libspectrum_word address )
{
  GSList *ptr;

  int found = 0;

  while( 1 ) {

    ptr = g_slist_find_custom( debugger_breakpoints, &address,
			       find_breakpoint_by_address );
    if( !ptr ) break;

    found++;

    debugger_breakpoints = g_slist_remove( debugger_breakpoints, ptr->data );
    if( debugger_mode == DEBUGGER_MODE_ACTIVE && !debugger_breakpoints )
      debugger_mode = DEBUGGER_MODE_INACTIVE;

    free( ptr->data );
  }

  if( !found ) {
    if( debugger_output_base == 10 ) {
      ui_error( UI_ERROR_ERROR, "No breakpoint at %d", address );
    } else {
      ui_error( UI_ERROR_ERROR, "No breakpoint at 0x%04x", address );
    }
  }

  return 0;
}

static gint
find_breakpoint_by_address( gconstpointer data, gconstpointer user_data )
{
  const debugger_breakpoint *bp = data;
  libspectrum_word address = *(const libspectrum_word*)user_data;

  /* Ignore all page-specific breakpoints */
  if( bp->page != -1 ) return 1;

  if( bp->type != DEBUGGER_BREAKPOINT_TYPE_EXECUTE &&
      bp->type != DEBUGGER_BREAKPOINT_TYPE_READ    &&
      bp->type != DEBUGGER_BREAKPOINT_TYPE_WRITE      )
    return 1;

  return bp->value - address;
}

/* Remove all breakpoints */
int
debugger_breakpoint_remove_all( void )
{
  g_slist_foreach( debugger_breakpoints, free_breakpoint, NULL );
  g_slist_free( debugger_breakpoints ); debugger_breakpoints = NULL;

  if( debugger_mode == DEBUGGER_MODE_ACTIVE )
    debugger_mode = DEBUGGER_MODE_INACTIVE;

  /* Restart the breakpoint numbering */
  next_breakpoint_id = 1;

  return 0;
}

static void
free_breakpoint( gpointer data, gpointer user_data GCC_UNUSED )
{
  debugger_breakpoint *bp = data;

  if( bp->condition ) debugger_expression_delete( bp->condition );

  free( bp );
}

/* Exit from the last CALL etc by setting a oneshot breakpoint at
   (SP) and then starting emulation */
int
debugger_breakpoint_exit( void )
{
  libspectrum_word target;

  target = readbyte_internal( SP ) + 0x100 * readbyte_internal( SP+1 );

  if( debugger_breakpoint_add( DEBUGGER_BREAKPOINT_TYPE_EXECUTE, -1, target, 0,
			       DEBUGGER_BREAKPOINT_LIFE_ONESHOT, NULL ) )
    return 1;

  if( debugger_run() ) return 1;

  return 0;
}

/* Ignore breakpoint 'id' the next 'ignore' times it hits */
int
debugger_breakpoint_ignore( size_t id, size_t ignore )
{
  debugger_breakpoint *bp;

  bp = get_breakpoint_by_id( id ); if( !bp ) return 1;

  bp->ignore = ignore;

  return 0;
}

/* Set the breakpoint's conditional expression */
int
debugger_breakpoint_set_condition( size_t id, debugger_expression *condition )
{
  debugger_breakpoint *bp;

  bp = get_breakpoint_by_id( id ); if( !bp ) return 1;

  if( bp->condition ) debugger_expression_delete( bp->condition );

  bp->condition = condition;

  return 0;
}

/* Poke a value into RAM */
int
debugger_poke( libspectrum_word address, libspectrum_byte value )
{
  writebyte_internal( address, value );
  return 0;
}

/* Write a value to a port */
int
debugger_port_write( libspectrum_word port, libspectrum_byte value )
{
  writeport( port, value );
  return 0;
}

/* Add events corresponding to all the time events to happen during
   this frame */
int
debugger_add_time_events( void )
{
  g_slist_foreach( debugger_breakpoints, add_time_event, NULL );
  return 0;
}

static void
add_time_event( gpointer data, gpointer user_data GCC_UNUSED )
{
  debugger_breakpoint *bp = data;

  if( bp->type == DEBUGGER_BREAKPOINT_TYPE_TIME )
    event_add( bp->value, EVENT_TYPE_BREAKPOINT );
}

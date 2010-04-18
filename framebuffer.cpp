#include "framebuffer.hpp"
#include "mutexobj.hpp"
#include "picture.hpp"

FrameQueue::FrameQueue( void )
{
  first = last = NULL;
  unixassert( pthread_mutex_init( &mutex, NULL ) );
}

void FrameQueue::add( Frame *frame )
{
  MutexLock x( &mutex );

  frame->prev = last;
  frame->next = NULL;

  if ( last ) {
    last->next = frame;
    last = frame;
  } else {
    first = last = frame;
  }
}

Frame *FrameQueue::remove( void )
{
  MutexLock x( &mutex );

  if ( first == NULL ) { /* empty */
    return NULL;
  }

  Frame *return_value = first;

  first = first->next;

  if ( first ) {
    first->prev = NULL;
  } else {
    last = NULL;
  }

  return_value->prev = return_value->next = NULL;

  return return_value;
}

void FrameQueue::remove_specific( Frame *frame )
{
  MutexLock x( &mutex );

  if ( frame->prev ) {
    frame->prev->next = frame->next;
  } else {
    first = frame->next;
  }

  if ( frame->next ) {
    frame->next->prev = frame->prev;
  } else {
    last = frame->prev;
  }

  frame->prev = frame->next = NULL;
}

BufferPool::BufferPool( uint s_num_frames, uint mb_width, uint mb_height )
{
  num_frames = s_num_frames;
  width = 16 * mb_width;
  height = 16 * mb_height;

  frames = new Frame *[ num_frames ];
  for ( uint i = 0; i < num_frames; i++ ) {
    frames[ i ] = new Frame( mb_width, mb_height );
    free.add( frames[ i ] );
  }

  unixassert( pthread_mutex_init( &mutex, NULL ) );
  unixassert( pthread_cond_init( &new_freeable, NULL ) );
}

BufferPool::~BufferPool()
{
  for ( uint i = 0; i < num_frames; i++ ) {
    Frame *frame = frames[ i ];
    delete frame;
  }
  delete[] frames;

  unixassert( pthread_cond_destroy( &new_freeable ) );
  unixassert( pthread_mutex_destroy( &mutex ) );
}

Frame::Frame( uint mb_width, uint mb_height )
{
  width = 16 * mb_width;
  height = 16 * mb_height;
  buf = new uint8_t[ sizeof( uint8_t ) * (3 * width * height / 2) ];
  state = FREE;
  handle = NULL;
  unixassert( pthread_mutex_init( &mutex, NULL ) );
  unixassert( pthread_cond_init( &activity, NULL ) );

  slicerow = new SliceRow *[ mb_height ];

  for ( uint i = 0; i < mb_height; i++ ) {
    slicerow[ i ] = new SliceRow( i, mb_height );
  }
}

Frame::~Frame()
{
  delete[] buf;

  for ( uint i = 0; i < height / 16; i++ ) {
    delete slicerow[ i ];
  }

  delete[] slicerow;

  unixassert( pthread_cond_destroy( &activity ) );
  unixassert( pthread_mutex_destroy( &mutex ) );
}

void Frame::lock( FrameHandle *s_handle,
		  int f_code_fv, int f_code_bv,
		  Picture *forward, Picture *backward )
{
  MutexLock x( &mutex );

  ahabassert( handle == NULL );
  ahabassert( state == FREE );
  handle = s_handle;
  state = LOCKED;

  for ( uint i = 0; i < height / 16; i++ ) {
    slicerow[ i ]->init( f_code_fv, f_code_bv, forward, backward );
  }
}

void Frame::set_rendered( void )
{
  MutexLock x( &mutex );

  ahabassert( state == LOCKED );
  state = RENDERED;
  pthread_cond_broadcast( &activity );
}

void Frame::relock( void )
{
  MutexLock x( &mutex );

  ahabassert( state == FREEABLE );
  state = RENDERED;
  pthread_cond_broadcast( &activity );
}

void Frame::set_freeable( void )
{
  MutexLock x( &mutex );

  ahabassert( state == RENDERED );
  state = FREEABLE;
}

void Frame::free_locked( void )
{
  MutexLock x( &mutex );

  ahabassert( state == LOCKED );
  /* handle->set_frame( NULL ); */ /* handle takes care of this */
  handle = NULL;
  state = FREE;
}

void Frame::free( void )
{
  MutexLock x( &mutex );

  ahabassert( state == FREEABLE );
  handle->set_frame( NULL );
  handle = NULL;
  state = FREE;
}

FrameHandle::FrameHandle( BufferPool *s_pool, Picture *s_pic )
{
  pool = s_pool;
  pic = s_pic;
  frame = NULL;
  locks = 0;
  unixassert( pthread_mutex_init( &mutex, NULL ) );
  unixassert( pthread_cond_init( &activity, NULL ) );
}

FrameHandle::~FrameHandle()
{
  unixassert( pthread_cond_destroy( &activity ) );
  unixassert( pthread_mutex_destroy( &mutex ) );
}

void FrameHandle::increment_lockcount( void )
{
  MutexLock x( &mutex );

  if ( frame ) {
    if ( locks == 0 ) {
      ahabassert( frame->get_state() == FREEABLE );
      pool->remove_from_freeable( frame );
      frame->relock();
    }
    locks++;
  } else {
    ahabassert( locks == 0 );
    frame = pool->get_free_frame();
    frame->lock( this, pic->get_f_code_fv(), pic->get_f_code_bv(),
		 pic->get_forward(), pic->get_backward() );
    locks++;
    pthread_cond_broadcast( &activity );
  }
}

void FrameHandle::decrement_lockcount( void )
{
  MutexLock x( &mutex );

  ahabassert( locks > 0 );
  locks--;
  if ( locks == 0 ) {
    if ( frame->get_state() == RENDERED ) {
      pool->make_freeable( frame );
      frame->set_freeable();
    } else if ( frame->get_state() == LOCKED ) {
      pool->make_free( frame );
      frame->free_locked();
      frame = NULL;
    } else {
      throw AhabException();
    }
  }
}

Frame *BufferPool::get_free_frame( void )
{
  MutexLock x( &mutex );

  Frame *first_free = free.remove();
  if ( first_free ) {
    return first_free;
  }

  Frame *first_freeable = freeable.remove();
  if ( !first_freeable ) {
    
    throw OutOfFrames();
  }
  first_freeable->free();

  return first_freeable;
}

void BufferPool::make_freeable( Frame *frame )
{
  MutexLock x( &mutex );
  freeable.add( frame );
}

void BufferPool::make_free( Frame *frame )
{
  MutexLock x( &mutex );
  free.add( frame );
}

void BufferPool::remove_from_freeable( Frame *frame )
{
  MutexLock x( &mutex );
  freeable.remove_specific( frame );
}

void FrameHandle::set_frame( Frame *s_frame )
{
  MutexLock x( &mutex );
  ahabassert( locks == 0 );
  frame = s_frame;
  pthread_cond_broadcast( &activity );
}

void Frame::wait_rendered( void )
{
  MutexLock x( &mutex );
  while ( state != RENDERED ) {
    pthread_cond_wait( &activity, &mutex );
  }
}

void FrameHandle::wait_rendered( void )
{
  MutexLock x( &mutex );

  while ( !frame ) {
    pthread_cond_wait( &activity, &mutex );
  }
  /* now we have a frame and our mutex is locked so it can't be taken away */

  frame->wait_rendered();
}

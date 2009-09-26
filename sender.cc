#include <pthread.h>
#include <vector>
#include <netdb.h>
#include "io.h"
#include "parse_args.h"
#include "cache.h"
#include "vw.h"
#include "simple_label.h"
#include "network.h"
#include "multisource.h"
#include "train_ring.h"

pthread_t* thread;
size_t d_1;
size_t d_2;
v_array<v_array<io_buf> > bufs;
bool second_of_pair[256];
bool pairs_exist=false;

size_t log_int_r(size_t v)
{
  if (v > 1)
    return 1+log_int_r(v >> 1);
  else
    return 0;
}

size_t log_int(size_t v)
{//find largest number n such that 2^n <= v
  return log_int_r(v);
}

size_t find_split(size_t number)
{/* Breaks <number> things into a factor of 2 by a factor of 2, with the first dimension longest */
  d_1 = 1;
  d_2 = 1;
  if (number > 1)
    {
      size_t log_2 = log_int(number);
      if (!pairs_exist)
	d_1 = 1 << log_2;
      else
	{
	  d_1 = 1 << (log_2 -1);
	  if (d_1 * d_1 > number)
	    d_2 = d_1 / 2;
	  else {
	    d_2 = d_1;
	    if (d_1 * 2 * d_2 < number)
	      d_1 *=2;
	  }
	}
      if (d_1 * d_2 < number)
	cerr << "warning: number of remote hosts is not a factor of 2, so some are wasted" << endl;
      return log_2;
    }
  return 0;
}

void open_sockets(vector<string>& hosts)
{
  for (size_t i = 0; i < d_1; i++)
    {
      v_array<io_buf> t;
      push(bufs,t);
      size_t new_id = global.unique_id;
      for (size_t j = 0; j< d_2; j++)
	{
	  size_t number = j + d_2*i;
	  int sd = open_socket(hosts[number], new_id);
	  if (new_id == 0)
	    global.local_prediction = sd;
	  new_id++;
	  io_buf b;
	  push(b.files, sd);
	  push(bufs[i], b);
	}
    }
}

void parse_send_args(po::variables_map& vm, vector<string> pairs, size_t& thread_bits)
{
  if (vm.count("sendto"))
    {      
      if (pairs.size() > 0)
	{
	  pairs_exist=true;
	  for (int i = 0; i<256;i++)
	    second_of_pair[i]=false;
	  for (vector<string>::iterator i = pairs.begin(); i != pairs.end();i++) 
	    second_of_pair[(int)(*i)[1]] = true;
	}

      vector<string> hosts = vm["sendto"].as< vector<string> >();
      thread_bits = max(thread_bits,find_split(hosts.size()));
      open_sockets(hosts);
    }
}

void send_features(int i, int j, io_buf& b, example* ec)
{
  output_int(bufs[i][j],ec->indices.index());
  
  for (size_t* index = ec->indices.begin; index != ec->indices.end; index++)
    if (second_of_pair[*index])
      output_features(b, *index, ec->subsets[*index][j*d_1], ec->subsets[*index][(j+1)*d_1]);
    else
      output_features(b, *index, ec->subsets[*index][i*d_2], ec->subsets[*index][(i+1)*d_2]);
  b.flush();
}

bool check_mesg(int sock)
{
  while (true)
    {
      prediction ps;
      if (get_prediction(sock,ps))
	{
	  example* ec = get_train_example(0);
	  ec->final_prediction = ps.p;
	  finish_example(ec);
	}
      else
	return false;
    }
}

void* send_thread(void*)
{
  example* ec = NULL;

  while ( true )
    {//this is a poor man's select operation.
      if (check_mesg(global.local_prediction))//nonblocking
	;
      else if ((ec = get_example(0)) != NULL)//blocking operation.
	{
	  label_data* ld = (label_data*)ec->ld;
	  
	  for (size_t i = 0; i < d_1; i++)
	    for (size_t j = 0; j < d_2; j++)
	      {
		simple_label.cache_label(ld,bufs[i][j]);//send label information.
		send_features(i,j,bufs[i][j],ec);
	      }
	  insert_example(ec);
	  ec->threads_to_finish = 1;
	  ec->done = true;
	}
      else if (thread_done(0))
	return NULL;
      else 
	;//busywait when we have sent out all examples but not yet received predictions for all.
    }

  return NULL;
}

void setup_send()
{
  thread = (pthread_t*)calloc(1,sizeof(pthread_t));
  
  pthread_create(thread, NULL, send_thread, NULL);
}

void destroy_send()
{
  pthread_join(*thread, NULL);
}

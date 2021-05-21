// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#pragma once

#include <functional>
#include <thread>
#include <future>
#include <mutex>  
#include "io_to_queue.h"

#include "io/logger.h"

using dispatch_fptr = std::function<void(vw&, const v_array<example*>&)>;
struct io_state;

inline void parse_dispatch(vw& all, dispatch_fptr dispatch)
{
  v_array<example*> examples;
  size_t example_number = 0;  // for variable-size batch learning algorithms

  // for substring_to_example
  std::vector<VW::string_view> words_localcpy;
  std::vector<VW::string_view> parse_name_localcpy;

  try
  {
    while (!all.example_parser->done)
    {

      example* example_ptr = &VW::get_unused_example(&all);
      examples.push_back(example_ptr);

      if (!all.do_reset_source && example_number != all.pass_length && all.max_examples > example_number &&
          all.example_parser->reader(&all, examples, words_localcpy, parse_name_localcpy) > 0)
      {

        VW::setup_examples(all, examples);
        example_number += examples.size();

        dispatch(all, examples);

      }
      // To make sure that in the end(when it is the last pass), the thread doesn't enter this block if the end_pass example is already taken care of by some other thread.
      // If this condition is not used, then this block can be executed more than once for the same pass (if the end_pass thread completes it's job and releases the lock, the current thread could have acquired the lock)
      else if(all.example_parser->current_pass_index != all.example_parser->next_pass_index && !all.example_parser->last_pass_complete)
      {
        // At this point, the current and next pass index become same. Which means the parser threads cannot enter this block.
        all.example_parser->current_pass_index++;

        reset_source(all, all.num_bits);

        // to call reset source in io thread
        all.example_parser->done_with_io.store(true);
        all.example_parser->can_end_pass.notify_one();

        all.do_reset_source = false;
        all.passes_complete++;

        // setup an end_pass example
        all.example_parser->lbl_parser.default_label(&examples[0]->l);
        examples[0]->end_pass = true;
        all.example_parser->in_pass_counter = 0;

        if (all.passes_complete == all.numpasses && example_number == all.pass_length)
        {
          all.passes_complete = 0;
          all.pass_length = all.pass_length * 2 + 1;
        }

        dispatch(all, examples);  // must be called before lock_done or race condition exists.
        if (all.passes_complete >= all.numpasses && all.max_examples >= example_number) {
          all.example_parser->last_pass_complete.store(true);
          lock_done(*all.example_parser);
        }
        example_number = 0;

        // To notify the other parser threads that they can continue their job.
        all.example_parser->next_pass_index++;
        all.example_parser->can_end_pass_parser.notify_all();

      }

      else{
        VW::finish_example(all, *example_ptr);

        // Stop other parser threads from executing till the pass has ended.
        std::mutex mut;
        std::unique_lock<std::mutex> lock(mut);
        while(all.example_parser->current_pass_index != all.example_parser->next_pass_index && !all.example_parser->last_pass_complete) {
          all.example_parser->can_end_pass_parser.wait(lock);
        }
      }

      examples.clear();
    }

  }
  
  catch (VW::vw_exception& e)
  {
    VW::io::logger::errlog_error("vw example #{0}({1}:{2}): {3}", example_number, e.Filename(), e.LineNumber(), e.what());

    // Stash the exception so it can be thrown on the main thread.
    all.example_parser->exc_ptr = std::current_exception();
  }
  catch (std::exception& e)
  {
    VW::io::logger::errlog_error("vw: example #{0}{1}", example_number, e.what());

    // Stash the exception so it can be thrown on the main thread.
    all.example_parser->exc_ptr = std::current_exception();
  }
  lock_done(*all.example_parser);
}

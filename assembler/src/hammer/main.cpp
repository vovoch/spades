//***************************************************************************
//* Copyright (c) 2011-2013 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * main.cpp
 *
 *  Created on: 08.07.2011
 *      Author: snikolenko
 */


#include "config_struct_hammer.hpp"
#include "hammer_tools.hpp"
#include "kmer_cluster.hpp"
#include "globals.hpp"
#include "kmer_data.hpp"
#include "expander.hpp"

#include "adt/concurrent_dsu.hpp"
#include "segfault_handler.hpp"
#include "io/read_processor.hpp"
#include "io/ireadstream.hpp"

#include "memory_limit.hpp"

#include "logger/logger.hpp"
#include "logger/log_writers.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <cassert>
#include <cmath>
#include <cstdlib>

std::vector<uint32_t> * Globals::subKMerPositions = NULL;
KMerData *Globals::kmer_data = NULL;
int Globals::iteration_no = 0;

char Globals::char_offset = 0;
bool Globals::char_offset_user = true;

double Globals::quality_probs[256] = { 0 };
double Globals::quality_lprobs[256] = { 0 };
double Globals::quality_rprobs[256] = { 0 };
double Globals::quality_lrprobs[256] = { 0 };

struct UfCmp {
  bool operator()(const std::vector<int> &lhs, const std::vector<int> &rhs) {
    return (lhs[0] < rhs[0]);
  }
};

void create_console_logger() {
  using namespace logging;

  logger *lg = create_logger("");
  lg->add_writer(std::make_shared<console_writer>());
  attach_logger(lg);
}

int main(int argc, char * argv[]) {
  segfault_handler sh;

  srand(42);
  srandom(42);

  try {
    create_console_logger();

    string config_file = CONFIG_FILENAME;
    if (argc > 1) config_file = argv[1];
    INFO("Loading config from " << config_file.c_str());
    cfg::create_instance(config_file);

    // hard memory limit
    const size_t GB = 1 << 30;
    limit_memory(cfg::get().general_hard_memory_limit * GB);

    // determine quality offset if not specified
    if (!cfg::get().input_qvoffset_opt) {
      INFO("Trying to determine PHRED offset");
      int determined_offset = determine_offset(*cfg::get().dataset.reads_begin());
      if (determined_offset < 0) {
        ERROR("Failed to determine offset! Specify it manually and restart, please!");
        return 0;
      } else {
        INFO("Determined value is " << determined_offset);
        cfg::get_writable().input_qvoffset = determined_offset;
      }
      Globals::char_offset_user = false;
    } else {
      cfg::get_writable().input_qvoffset = *cfg::get().input_qvoffset_opt;
      Globals::char_offset_user = true;
    }
    Globals::char_offset = (char)cfg::get().input_qvoffset;

    // Pre-cache quality probabilities
    for (unsigned qual = 0; qual < sizeof(Globals::quality_probs) / sizeof(Globals::quality_probs[0]); ++qual) {
      Globals::quality_rprobs[qual] = (qual < 3 ? 0.75 : pow(10.0, -(int)qual / 10.0));
      Globals::quality_probs[qual] = 1 - Globals::quality_rprobs[qual];
      Globals::quality_lprobs[qual] = log(Globals::quality_probs[qual]);
      Globals::quality_lrprobs[qual] = log(Globals::quality_rprobs[qual]);
    }

    // initialize subkmer positions
    HammerTools::InitializeSubKMerPositions();

    int max_iterations = cfg::get().general_max_iterations;

    // now we can begin the iterations
    for (Globals::iteration_no = 0; Globals::iteration_no < max_iterations; ++Globals::iteration_no) {
      cout << "\n     === ITERATION " << Globals::iteration_no << " begins ===" << endl;
      bool do_everything = cfg::get().general_do_everything_after_first_iteration && (Globals::iteration_no > 0);

      // initialize k-mer structures
      Globals::kmer_data = new KMerData;

      // count k-mers
      if (cfg::get().count_do || do_everything) {
        KMerDataCounter(cfg::get().count_numfiles).BuildKMerIndex(*Globals::kmer_data);

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is, fname);
      }

      // Cluster the Hamming graph
      std::vector<std::vector<size_t> > classes;
      if (cfg::get().hamming_do || do_everything) {
        ConcurrentDSU uf(Globals::kmer_data->size());
        KMerHamClusterer clusterer(cfg::get().general_tau);
        INFO("Clustering Hamming graph.");
        clusterer.cluster(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamcls"),
                          *Globals::kmer_data, uf);
        uf.get_sets(classes);
        size_t num_classes = classes.size();

#if 0
        std::sort(classes.begin(), classes.end(),  UfCmp());
        for (size_t i = 0; i < classes.size(); ++i) {
          std::cerr << i << ": { ";
          for (size_t j = 0; j < classes[i].size(); ++j)
            std::cerr << classes[i][j] << ", ";
          std::cerr << "}" << std::endl;
        }
#endif
        INFO("Clustering done. Total clusters: " << num_classes);

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Writing down clusters.");

          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamming");
          std::ofstream ofs(fname.c_str(),
                            std::ios::binary | std::ios::out);

          ofs.write((char*)&num_classes, sizeof(num_classes));
          for (size_t i=0; i < classes.size(); ++i) {
            size_t sz = classes[i].size();
            ofs.write((char*)&sz, sizeof(sz));
            ofs.write((char*)&classes[i][0], sz * sizeof(classes[i][0]));
          }
        }
      } else {
        INFO("Reading clusters");

        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamming");
        std::ifstream is(fname.c_str(), std::ios::binary | std::ios::in);
        VERIFY(is.good());

        size_t num_classes = 0;
        is.read((char*)&num_classes, sizeof(num_classes));
        classes.resize(num_classes);

        for (size_t i = 0; i < num_classes; ++i) {
          size_t sz = 0;
          is.read((char*)&sz, sizeof(sz));
          classes[i].resize(sz);
          is.read((char*)&classes[i][0], sz * sizeof(classes[i][0]));
        }
      }

      if (cfg::get().bayes_do || do_everything) {
        KMerDataCounter(cfg::get().count_numfiles).FillKMerData(*Globals::kmer_data);

        INFO("Subclustering Hamming graph");
        unsigned clustering_nthreads = std::min(cfg::get().general_max_nthreads, cfg::get().bayes_nthreads);
        KMerClustering kmc(*Globals::kmer_data, clustering_nthreads, cfg::get().input_working_dir);
        kmc.process(classes);
        INFO("Finished clustering.");

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index2");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index2");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is, fname);
      }
      std::vector<std::vector<size_t>>().swap(classes);

      // expand the set of solid k-mers
      if (cfg::get().expand_do || do_everything) {
        unsigned expand_nthreads = std::min(cfg::get().general_max_nthreads, cfg::get().expand_nthreads);
        INFO("Starting solid k-mers expansion in " << expand_nthreads << " threads.");
        for (unsigned expand_iter_no = 0; expand_iter_no < cfg::get().expand_max_iterations; ++expand_iter_no) {
          Expander expander(*Globals::kmer_data);
          const io::DataSet<> &dataset = cfg::get().dataset;
          for (auto I = dataset.reads_begin(), E = dataset.reads_end(); I != E; ++I) {
            ireadstream irs(*I, cfg::get().input_qvoffset);
            hammer::ReadProcessor rp(expand_nthreads);
            rp.Run(irs, expander);
            VERIFY_MSG(rp.read() == rp.processed(), "Queue unbalanced");
          }

          if (cfg::get().expand_write_each_iteration) {
            std::ofstream oftmp(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "goodkmers", expand_iter_no).data());
            for (size_t n = 0; n < Globals::kmer_data->size(); ++n) {
              const KMerStat &kmer_data = (*Globals::kmer_data)[n];
              if (kmer_data.isGoodForIterative())
                oftmp << Globals::kmer_data->kmer(n).str() << "\n>" << n
                      << "  cnt=" << kmer_data.count << "  tql=" << (1-kmer_data.totalQual) << "\n";
            }
          }

          INFO("Solid k-mers iteration " << expand_iter_no << " produced " << expander.changed() << " new k-mers.");
          if (expander.changed() < 10)
            break;
        }
        INFO("Solid k-mers finalized");

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index3");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index3");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is, fname);
      }

      size_t totalReads = 0;
      // reconstruct and output the reads
      if (cfg::get().correct_do || do_everything) {
        totalReads = HammerTools::CorrectAllReads();
      }

      // prepare the reads for next iteration
      delete Globals::kmer_data;

      if (totalReads < 1) {
        INFO("Too few reads have changed in this iteration. Exiting.");
        break;
      }
      // break;
    }

    std::string fname = HammerTools::getFilename(cfg::get().output_dir, "corrected.yaml");
    INFO("Saving corrected dataset description to " << fname);
    cfg::get().dataset.save(fname);

    // clean up
    Globals::subKMerPositions->clear();
    delete Globals::subKMerPositions;

    INFO("All done. Exiting.");
  } catch (std::bad_alloc const& e) {
    std::cerr << "Not enough memory to run BayesHammer. " << e.what() << std::endl;
    return EINTR;
  } catch (const YAML::Exception &e) {
    std::cerr << "Error reading config file: " << e.what() << std::endl;
    return EINTR;
  } catch (std::exception const& e) {
    std::cerr << "Exception caught " << e.what() << std::endl;
    return EINTR;
  } catch (...) {
    std::cerr << "Unknown exception caught " << std::endl;
    return EINTR;
  }

  return 0;
}

#include <deque>
#include <iostream>
#include <string>
#include <tclap/CmdLine.h>

#include "version.hpp"
#include "Pore_Model.hpp"
#include "Builtin_Model.hpp"
#include "State_Transitions.hpp"
#include "Event.hpp"
#include "Fast5_Summary.hpp"
#include "Viterbi.hpp"
#include "Forward_Backward.hpp"
#include "Model_Parameter_Trainer.hpp"
#include "logger.hpp"
#include "alg.hpp"
#include "zstr.hpp"
#include "fast5.hpp"
#include "pfor.hpp"
#include "fs_support.hpp"

using namespace std;

namespace opts
{
    using namespace TCLAP;
    string description = "Call bases in Oxford Nanopore reads.";
    CmdLine cmd_parser(description, ' ', package_version);
    MultiArg< string > log_level("", "log", "Log level.", false, "string", cmd_parser);
    ValueArg< string > stats_fn("", "stats", "Stats.", false, "", "file", cmd_parser);
    ValueArg< unsigned > min_read_len("", "min-len", "Minimum read length.", false, 1000, "int", cmd_parser);
    ValueArg< unsigned > fasta_line_width("", "fasta-line-width", "Maximum fasta line width.", false, 80, "int", cmd_parser);
    //
    SwitchArg scale_select_model_single_round("", "scale-select-model-single-round", "Use a single round of FWBW to select best model per strand.", cmd_parser);
    SwitchArg scale_strands_together("", "scale-strands-together", "Use same scaling parameters for both strands.", cmd_parser);
    ValueArg< float > scale_min_fit_progress("", "scale-min-fit-progress", "Minimum scaling fit progress.", false, 1.0, "float", cmd_parser);
    ValueArg< unsigned > scale_max_rounds("", "scale-max-rounds", "Maximum scaling rounds.", false, 10, "int", cmd_parser);
    ValueArg< unsigned > scale_num_events("", "scale-num-events", "Number of events used for model scaling.", false, 200, "int", cmd_parser);
    SwitchArg scale_only("", "scale-only", "Stop after computing model scalings.", cmd_parser);
    SwitchArg accurate_scaling("", "accurate", "Compute model scalings more accurately.", cmd_parser);
    ValueArg< float > pr_cutoff("", "pr-cutoff", "Minimum value for transition probabilities; smaller values are set to zero.", false, .001, "float", cmd_parser);
    ValueArg< float > pr_skip("", "pr-skip", "Transition probability of skipping at least 1 state.", false, .1, "float", cmd_parser);
    ValueArg< float > pr_stay("", "pr-stay", "Transition probability of staying in the same state.", false, .1, "float", cmd_parser);
    ValueArg< string > trans_fn("s", "trans", "Custom initial state transitions.", false, "", "file", cmd_parser);
    ValueArg< string > model_fofn("", "model-fofn", "File of pore models.", false, "", "file", cmd_parser);
    MultiArg< string > model_fn("m", "model", "Custom pore model.", false, "file", cmd_parser);
    ValueArg< string > output_fn("o", "output", "Output.", false, "", "file", cmd_parser);
    ValueArg< unsigned > num_threads("t", "threads", "Number of parallel threads.", false, 1, "int", cmd_parser);
    UnlabeledMultiArg< string > input_fn("inputs", "Inputs. Accepts: directories, fast5 files, or files of fast5 file names (use \"-\" to read fofn from stdin).", true, "path", cmd_parser);
} // namespace opts

void init_models(Pore_Model_Dict_Type& models)
{
    auto parse_model_name = [] (const string& s) {
        if (s.size() < 3
            or (s[0] != '0' and s[0] != '1' and s[0] != '2')
            or s[1] != ':')
        {
            LOG(error) << "could not parse model name: \"" << s << "\"; format should be \"[0|1|2]:<file>\"" << endl;
            exit(EXIT_FAILURE);
        }
        unsigned st = s[0] - '0';
        return make_pair(st, s.substr(2));
    };

    map< unsigned, list< string > > model_list;
    if (not opts::model_fn.get().empty())
    {
        for (const auto& s : opts::model_fn.get())
        {
            auto p = parse_model_name(s);
            model_list[p.first].push_back(p.second);
        }
    }
    if (not opts::model_fofn.get().empty())
    {
        zstr::ifstream ifs(opts::model_fofn);
        string s;
        while (getline(ifs, s))
        {
            auto p = parse_model_name(s);
            model_list[p.first].push_back(p.second);
        }
    }
    if (model_list[2].empty() and (model_list[0].empty() != model_list[1].empty()))
    {
        LOG(error) << "models were specified only for strand " << (int)model_list[0].empty()
                   << "! give models for both strands, or for neither." << endl;
        exit(EXIT_FAILURE);
    }
    if (not (model_list[0].empty() and model_list[1].empty() and model_list[2].empty()))
    {
        for (unsigned st = 0; st < 3; ++st)
        {
            for (const auto& e : model_list[st])
            {
                Pore_Model_Type pm;
                string pm_name = e;
                zstr::ifstream(e) >> pm;
                pm.strand() = st;
                models[pm_name] = move(pm);
                LOG(info) << "loaded module [" << pm_name << "] for strand [" << st << "]" << endl;
            }
        }
    }
    else
    {
        // use built-in models
        for (unsigned i = 0; i < Builtin_Model::num; ++i)
        {
            Pore_Model_Type pm;
            string pm_name = Builtin_Model::names[i];
            pm.load_from_vector(Builtin_Model::init_lists[i]);
            pm.strand() = Builtin_Model::strands[i];
            models[Builtin_Model::names[i]] = move(pm);
            LOG(info)
                << "loaded builtin module [" << Builtin_Model::names[i] << "] for strand ["
                << Builtin_Model::strands[i] << "] statistics [mean=" << pm.mean() << ", stdv="
                << pm.stdv() << "]" << endl;
        }
    }
} // init_models

void init_transitions(State_Transitions_Type& transitions)
{
    if (not opts::trans_fn.get().empty())
    {
        zstr::ifstream(opts::trans_fn) >> transitions;
        LOG(info) << "loaded state transitions from [" << opts::trans_fn.get() << "]" << endl;
    }
    else
    {
        transitions.compute_transitions(opts::pr_skip, opts::pr_stay, opts::pr_cutoff);
        LOG(info) << "initialized state transitions with parameters p_skip=[" << opts::pr_skip
                  << "], pr_stay=[" << opts::pr_stay << "], pr_cutoff=[" << opts::pr_cutoff << "]" << endl;
    }
} // init_transitions

// Parse command line arguments. For each of them:
// - if it is a directory, find all fast5 files in it, ignore non-fast5 files.
// - if it is a file, check that it is indeed a fast5 file.
void init_files(list< string >& files)
{
    for (const auto& f : opts::input_fn)
    {
        if (is_directory(f))
        {
            auto l = list_directory(f);
            for (const auto& g : l)
            {
                string f2 = f + (f[f.size() - 1] != '/'? "/" : "") + g;
                if (is_directory(f2))
                {
                    LOG(info) << "ignoring subdirectory [" << f2 << "]" << endl;
                }
                else if (fast5::File::is_valid_file(f2))
                {
                    files.push_back(f2);
                    LOG(info) << "adding input file [" << f2 << "]" << endl;
                }
                else
                {
                    LOG(info) << "ignoring file [" << f2 << "]" << endl;
                }
            }
        }
        else // not a directory
        {
            if (f != "-" and fast5::File::is_valid_file(f))
            {
                files.push_back(f);
                LOG(info) << "adding input file [" << f << "]" << endl;
            }
            else // not fast5, interpret as fofn
            {
                LOG(info) << "interpreting [" << f << "] as fofn" << endl;
                istream* is_p = nullptr;
                strict_fstream::ifstream ifs;
                if (f == "-")
                {
                    is_p = &cin;
                }
                else
                {
                    ifs.open(f);
                    is_p = &ifs;
                }
                string g;
                while (getline(*is_p, g))
                {
                    if (fast5::File::is_valid_file(g))
                    {
                        files.push_back(g);
                        LOG(info) << "adding input file [" << g << "]" << endl;
                    }
                }
            }
        }
    }
    if (files.empty())
    {
        LOG(error) << "no fast5 files to process" << endl;
        exit(EXIT_FAILURE);
    }
} // init_files

void init_reads(const Pore_Model_Dict_Type& models,
                const list< string >& files,
                deque< Fast5_Summary_Type >& reads)
{
    for (const auto& f : files)
    {
        Fast5_Summary_Type s(f, models, opts::scale_strands_together);
        LOG(info) << "summary: " << s << endl;
        if (s.have_ed_events
            and (s.strand_bounds[1] >= s.strand_bounds[0] + opts::min_read_len
                 or s.strand_bounds[3] >= s.strand_bounds[2] + opts::min_read_len))
        {
            reads.emplace_back(move(s));
        }
    }
} // init_reads

void rescale_reads(const Pore_Model_Dict_Type& models,
                   State_Transitions_Type& transitions,
                   deque< Fast5_Summary_Type >& reads)
{
    unsigned crt_idx = 0;
    pfor::pfor< unsigned >(
        opts::num_threads,
        10,
        // get_item
        [&] (unsigned& i) {
            if (crt_idx >= reads.size()) return false;
            i = crt_idx++;
            return true;
        },
        // process item
        [&] (unsigned& i) {
            Fast5_Summary_Type& read_summary = reads[i];
            read_summary.load_events(opts::scale_strands_together);
            //
            // create per-strand list of models to try
            //
            array< list< string >, 2 > model_list;
            for (unsigned st = 0; st < 2; ++st)
            {
                // if not enough events, ignore strand
                if (read_summary.events[st].size() < opts::min_read_len) continue;
                // create list of models to try
                if (models.count(read_summary.preferred_model[st]))
                {
                    // if we have a preferred model, use that
                    model_list[st].push_back(read_summary.preferred_model[st]);
                }
                else
                {
                    // no preferred model, try all that apply to this strand
                    for (const auto& p : models)
                    {
                        if (p.second.strand() == st or p.second.strand() == 2)
                        {
                            model_list[st].push_back(p.first);
                        }
                    }
                }
                assert(not model_list.empty());
            }
            //
            // create per-strand list of event sequences on which to train
            //
            array< vector< Event_Sequence_Type >, 2 > train_event_seqs;
            for (unsigned st = 0; st < 2; ++st)
            {
                // if not enough events, ignore strand
                if (read_summary.events[st].size() < opts::min_read_len) continue;
                // create 2 event sequences on which to train
                unsigned n_events = min((size_t)opts::scale_num_events.get(), read_summary.events[st].size());
                train_event_seqs[st].emplace_back(
                    read_summary.events[st].begin(), read_summary.events[st].begin() + n_events / 2);
                train_event_seqs[st].emplace_back(
                    read_summary.events[st].end() - n_events / 2, read_summary.events[st].end());
            }
            if (opts::scale_strands_together
                and read_summary.events[0].size() >= opts::min_read_len
                and read_summary.events[1].size() >= opts::min_read_len)
            {
                // prepare vector of event sequences
                vector< const Event_Sequence_Type* > train_event_seq_ptrs;
                for (unsigned st = 0; st < 2; ++st)
                {
                    for (const auto& events : train_event_seqs[st])
                    {
                        train_event_seq_ptrs.push_back(&events);
                    }
                }
                // run fwbw for 1 round, update params
                map< pair< string, string >, float > model_fit;
                for (const auto& m_name_0 : model_list[0]) 
                {
                    for (const auto& m_name_1 : model_list[1])
                    {
                        auto m_name_str = m_name_0 + '+' + m_name_1;
                        auto m_name = make_pair(m_name_0, m_name_1);
                        // prepare vector of model pointers
                        vector< const Pore_Model_Type* > model_ptrs;
                        model_ptrs.insert(model_ptrs.end(), train_event_seqs[0].size(), &models.at(m_name_0));
                        model_ptrs.insert(model_ptrs.end(), train_event_seqs[1].size(), &models.at(m_name_1));
                        Pore_Model_Parameters_Type old_pm_params = read_summary.params[2].at(m_name_str);
                        Pore_Model_Parameters_Type crt_pm_params;
                        bool done;
                        Model_Parameter_Trainer_Type::train_one_round(
                            train_event_seq_ptrs, model_ptrs, transitions,
                            old_pm_params, crt_pm_params, model_fit[m_name], done);
                        LOG(debug)
                            << "scaling_round read [" << read_summary.read_id
                            << "] strand [" << 2
                            << "] model [" << m_name_str
                            << "] old_params [" << old_pm_params
                            << "] old_fit [" << -INFINITY
                            << "] crt_params [" << crt_pm_params
                            << "] crt_fit [" << model_fit[m_name]
                            << "] round [0]" << endl;
                        read_summary.params[2].at(m_name_str) = move(crt_pm_params);
                    }
                }
                if (true or opts::scale_select_model_single_round)
                {
                    // note: always select model here if scaling strands together
                    auto it_max = alg::max_of(
                        model_fit,
                        [] (const decltype(model_fit)::value_type& p) { return p.second; });
                    read_summary.preferred_model[0] = it_max->first.first;
                    read_summary.preferred_model[1] = it_max->first.second;
                    model_list[0].assign({it_max->first.first});
                    model_list[1].assign({it_max->first.second});
                    LOG(debug)
                        << "selected_model read [" << read_summary.read_id
                        << "] strand [2] model [" << it_max->first.first + '+' + it_max->first.second << "]" << endl;
                }
                for (const auto& m_name_0 : model_list[0]) 
                {
                    for (const auto& m_name_1 : model_list[1])
                    {
                        auto m_name_str = m_name_0 + '+' + m_name_1;
                        auto m_name = make_pair(m_name_0, m_name_1);
                        // prepare vector of model pointers
                        vector< const Pore_Model_Type* > model_ptrs;
                        model_ptrs.insert(model_ptrs.end(), train_event_seqs[0].size(), &models.at(m_name_0));
                        model_ptrs.insert(model_ptrs.end(), train_event_seqs[1].size(), &models.at(m_name_1));
                        unsigned round = 1;
                        Pore_Model_Parameters_Type crt_pm_params = read_summary.params[2].at(m_name_str);
                        float crt_fit = model_fit[m_name];
                        while (true)
                        {
                            Pore_Model_Parameters_Type old_pm_params(crt_pm_params);
                            float old_fit(crt_fit);
                            bool done;

                            Model_Parameter_Trainer_Type::train_one_round(
                                train_event_seq_ptrs, model_ptrs, transitions,
                                old_pm_params, crt_pm_params, crt_fit, done);

                            LOG(debug)
                                << "scaling_round read [" << read_summary.read_id
                                << "] strand [" << 2
                                << "] model [" << m_name_str
                                << "] old_params [" << old_pm_params
                                << "] old_fit [" << old_fit
                                << "] crt_params [" << crt_pm_params
                                << "] crt_fit [" << crt_fit
                                << "] round [" << round << "]" << endl;

                            if (done)
                            {
                                // singularity detected; stop
                                break;
                            }

                            if (crt_fit < old_fit)
                            {
                                LOG(info) << "scaling_regression read [" << read_summary.read_id
                                          << "] strand [" << 2
                                          << "] model [" << m_name_str
                                          << "] old_params [" << old_pm_params
                                          << "] old_fit [" << old_fit
                                          << "] crt_params [" << crt_pm_params
                                          << "] crt_fit [" << crt_fit
                                          << "] round [" << round << "]" << endl;
                                crt_pm_params = old_pm_params;
                                crt_fit = old_fit;
                                break;
                            }

                            ++round;
                            // stop condition
                            if (round >= opts::scale_max_rounds
                                or (round > 1 and crt_fit < old_fit + opts::scale_min_fit_progress))
                            {
                                break;
                            }

                        }; // while true
                        LOG(info)
                            << "scaling_result read [" << read_summary.read_id
                            << "] strand [" << 2
                            << "] model [" << m_name_str
                            << "] parameters [" << crt_pm_params
                            << "] fit [" << crt_fit
                            << "] rounds [" << round << "]" << endl;
                        read_summary.params[2].at(m_name_str) = crt_pm_params;
                        read_summary.params[0][m_name_0] = crt_pm_params;
                        read_summary.params[1][m_name_1] = crt_pm_params;
                    } // for m_name[1]
                } // for m_name[0]
            }
            else // not opts::scale_strands_together
            {
                for (unsigned st = 0; st < 2; ++st)
                {
                    // if not enough events, ignore strand
                    if (read_summary.events[st].size() < opts::min_read_len) continue;
                    vector< const Event_Sequence_Type* > train_event_seq_ptrs;
                    for (const auto& events : train_event_seqs[st])
                    {
                        train_event_seq_ptrs.push_back(&events);
                    }
                    // run fwbw for 1 round and update params
                    map< string, float > model_fit;
                    for (const auto& m_name : model_list[st])
                    {
                        Pore_Model_Parameters_Type old_pm_params = read_summary.params[st].at(m_name);
                        Pore_Model_Parameters_Type crt_pm_params;
                        bool done;
                        Model_Parameter_Trainer_Type::train_one_round(
                            train_event_seq_ptrs, { &models.at(m_name) }, transitions,
                            old_pm_params, crt_pm_params, model_fit[m_name], done);
                        LOG(debug)
                            << "scaling_round read [" << read_summary.read_id
                            << "] strand [" << st
                            << "] model [" << m_name
                            << "] old_params [" << old_pm_params
                            << "] old_fit [" << -INFINITY
                            << "] crt_params [" << crt_pm_params
                            << "] crt_fit [" << model_fit[m_name]
                            << "] round [0]" << endl;
                        read_summary.params[st].at(m_name) = move(crt_pm_params);
                    }
                    if (opts::scale_select_model_single_round)
                    {
                        auto it_max = alg::max_of(
                            model_fit,
                            [] (const decltype(model_fit)::value_type& p) { return p.second; });
                        read_summary.preferred_model[st] = it_max->first;
                        model_list[st].assign({it_max->first});
                        LOG(debug)
                            << "selected_model read [" << read_summary.read_id
                            << "] strand [" << st
                            << "] model [" << it_max->first << "]" << endl;
                    }
                    // continue remaining training rounds
                    for (const auto& m_name : model_list[st])
                    {
                        unsigned round = 1;
                        Pore_Model_Parameters_Type crt_pm_params = read_summary.params[st].at(m_name);
                        float crt_fit = model_fit[m_name];
                        while (true)
                        {
                            Pore_Model_Parameters_Type old_pm_params(crt_pm_params);
                            float old_fit(crt_fit);
                            bool done;

                            Model_Parameter_Trainer_Type::train_one_round(
                                train_event_seq_ptrs, { &models.at(m_name) }, transitions,
                                old_pm_params, crt_pm_params, crt_fit, done);

                            LOG(debug)
                                << "scaling_round read [" << read_summary.read_id
                                << "] strand [" << st
                                << "] model [" << m_name
                                << "] old_params [" << old_pm_params
                                << "] old_fit [" << old_fit
                                << "] crt_params [" << crt_pm_params
                                << "] crt_fit [" << crt_fit
                                << "] round [" << round << "]" << endl;

                            if (done)
                            {
                                // singularity detected; stop
                                break;
                            }

                            if (crt_fit < old_fit)
                            {
                                LOG(info) << "scaling_regression read [" << read_summary.read_id
                                          << "] strand [" << st
                                          << "] model [" << m_name
                                          << "] old_params [" << old_pm_params
                                          << "] old_fit [" << old_fit
                                          << "] crt_params [" << crt_pm_params
                                          << "] crt_fit [" << crt_fit
                                          << "] round [" << round << "]" << endl;
                                crt_pm_params = old_pm_params;
                                crt_fit = old_fit;
                                break;
                            }

                            ++round;
                            // stop condition
                            if (round >= opts::scale_max_rounds
                                or (round > 1 and crt_fit < old_fit + opts::scale_min_fit_progress))
                            {
                                break;
                            }

                        }; // while true
                        LOG(info)
                            << "scaling_result read [" << read_summary.read_id
                            << "] strand [" << st
                            << "] model [" << m_name
                            << "] parameters [" << crt_pm_params
                            << "] fit [" << crt_fit
                            << "] rounds [" << round << "]" << endl;
                        read_summary.params[st].at(m_name) = move(crt_pm_params);
                    } // for m_name
                } // for st
            } // if not opts::scale_strands_together

            read_summary.drop_events();
        }, // process_item
        // progress_report
        [&] (unsigned items, unsigned seconds) {
            clog << "Processed " << setw(6) << right << items << " reads in "
                 << setw(6) << right << seconds << " seconds\r";
        }); // pfor
} // rescale_reads

void write_fasta(ostream& os, const string& name, const string& seq)
{
    os << ">" << name << endl;
    for (unsigned pos = 0; pos < seq.size(); pos += opts::fasta_line_width)
    {
        os << seq.substr(pos, opts::fasta_line_width) << endl;
    }
} // write_fasta

void basecall_reads(const Pore_Model_Dict_Type& models,
                    const State_Transitions_Type& transitions,
                    deque< Fast5_Summary_Type >& reads)
{
    strict_fstream::ofstream ofs;
    ostream* os_p = nullptr;
    if (not opts::output_fn.get().empty())
    {
        ofs.open(opts::output_fn);
        os_p = &ofs;
    }
    else
    {
        os_p = &cout;
    }

    unsigned crt_idx = 0;
    pfor::pfor< unsigned, ostringstream >(
        opts::num_threads,
        10,
        // get_item
        [&] (unsigned& i) {
            if (crt_idx >= reads.size()) return false;
            i = crt_idx++;
            return true;
        },
        // process_item
        [&] (unsigned& i, ostringstream& oss) {
            Fast5_Summary_Type& read_summary = reads[i];
            read_summary.load_events(opts::scale_strands_together);
            for (unsigned st = 0; st < 2; ++st)
            {
                // if not enough events, ignore strand
                if (read_summary.events[st].size() < opts::min_read_len) continue;
                // create list of models to try
                list< string > model_sublist;
                if (models.count(read_summary.preferred_model[st]))
                {
                    // if we have a preferred model, use that
                    model_sublist.push_back(read_summary.preferred_model[st]);
                }
                else
                {
                    // no preferred model, try all that apply to this strand
                    for (const auto& p : models)
                    {
                        if (p.second.strand() == st or p.second.strand() == 2)
                        {
                            model_sublist.push_back(p.first);
                        }
                    }
                }
                // check main scaling parameters
                auto r = alg::mean_stdv_of< float >(
                    read_summary.events[st],
                    [] (const Event_Type& ev) { return ev.mean; });
                LOG(debug)
                    << "mean_stdv read [" << read_summary.read_id
                    << "] strand [" << st
                    << "] ev_mean=[" << r.first
                    << "] ev_stdv=[" << r.second << "]" << endl;
                // deque of results
                deque< tuple< float, string, string > > results;
                for (const auto& m_name : model_sublist)
                {
                    // scale model, initialize default parameters if necessary
                    Pore_Model_Type pm(models.at(m_name));
                    Pore_Model_Parameters_Type pm_params = read_summary.params[st].at(m_name);
                    pm.scale(pm_params);
                    LOG(info)
                        << "basecalling read [" << read_summary.read_id
                        << "] strand [" << st
                        << "] model [" << m_name
                        << "] parameters " << pm_params << endl;
                    LOG(debug)
                        << "mean_stdv read [" << read_summary.read_id
                        << "] strand [" << st
                        << "] model_mean [" << pm.mean()
                        << "] model_stdv [" << pm.stdv() << "]" << endl;
                    if (abs(r.first - pm.mean()) > 5.0)
                    {
                        LOG(warning)
                            << "means_apart read [" << read_summary.read_id
                            << "] strand [" << st
                            << "] model [" << m_name
                            << "] parameters " << read_summary.params[st].at(m_name)
                            << " model_mean=[" << pm.mean()
                            << "] events_mean=[" << r.first
                            << "]" << endl;
                    }
                    // correct drift
                    Event_Sequence_Type corrected_events = read_summary.events[st];
                    corrected_events.apply_drift_correction(pm_params.drift);
                    Viterbi_Type vit;
                    vit.fill(pm, transitions, corrected_events);
                    results.emplace_back(make_tuple(vit.path_probability(), m_name, vit.base_seq()));
                }
                sort(results.begin(), results.end());
                string& best_m_name = get<1>(results.back());
                string& base_seq = get<2>(results.back());
                LOG(info)
                    << "best_model read [" << read_summary.read_id
                    << "] strand [" << st
                    << "] model [" << best_m_name
                    << "] parameters " << read_summary.params[st].at(best_m_name) << endl;
                read_summary.preferred_model[st] = best_m_name;
                ostringstream tmp;
                tmp << read_summary.read_id << ":" << read_summary.base_file_name << ":" << st;
                write_fasta(oss, tmp.str(), base_seq);
            } // for st
            read_summary.drop_events();
        },
        // output_chunk
        [&] (ostringstream& oss) {
            *os_p << oss.str();
        },
        // progress_report
        [&] (unsigned items, unsigned seconds) {
            clog << "Processed " << setw(6) << right << items << " reads in "
                 << setw(6) << right << seconds << " seconds\r";
        }); // pfor
} // basecall_reads

int real_main()
{
    Pore_Model_Dict_Type models;
    State_Transitions_Type transitions;
    deque< Fast5_Summary_Type > reads;
    list< string > files;
    // initialize structs
    init_models(models);
    init_transitions(transitions);
    init_files(files);
    init_reads(models, files, reads);
    if (opts::accurate_scaling or opts::scale_strands_together or opts::scale_select_model_single_round)
    {
        // do some rescaling
        rescale_reads(models, transitions, reads);
    }
    if (not opts::scale_only)
    {
        // basecall reads
        basecall_reads(models, transitions, reads);
    }
    // print stats
    if (not opts::stats_fn.get().empty())
    {
        strict_fstream::ofstream ofs(opts::stats_fn);
        for (const auto& s : reads)
        {
            s.write_tsv(ofs);
            ofs << endl;
        }
    }
    return EXIT_SUCCESS;
}

int main(int argc, char * argv[])
{
    opts::cmd_parser.parse(argc, argv);
    logger::Logger::set_default_level(logger::level::info);
    logger::Logger::set_levels_from_options(opts::log_level);
    Fast5_Summary_Type::min_read_len() = opts::min_read_len;
#ifndef H5_HAVE_THREADSAFE
    if (opts::num_threads > 1)
    {
        LOG(warning) << "enabled multi-threading with non-threadsafe HDF5: using experimental locking" << endl;
    }
#endif
    LOG(info) << "program: " << opts::cmd_parser.getProgramName() << endl;
    LOG(info) << "version: " << opts::cmd_parser.getVersion() << endl;
    LOG(info) << "args: " << opts::cmd_parser.getOrigArgv() << endl;
    return real_main();
}

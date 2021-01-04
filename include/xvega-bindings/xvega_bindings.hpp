/***************************************************************************
* Copyright (c) 2020, QuantStack and xeus-SQLite contributors              *
*                                                                          *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#ifndef XVEGA_BINDINGS_HPP
#define XVEGA_BINDINGS_HPP

#include <algorithm>
#include <any>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <utility>

#include <cxxabi.h>

#include "nlohmann/json.hpp"
#include "xvega/xvega.hpp"

#include "utils.hpp"

namespace nl = nlohmann;

namespace xv_bindings
{
    std::string demangle(const char* mangled)
    {
          int status;
          std::unique_ptr<char[], void (*)(void*)> result(
            __cxxabiv1::__cxa_demangle(mangled, 0, 0, &status), std::free);
          return result.get() ? std::string(result.get()) : "error occurred";
    }
    /**
        Base parser class, should be inherited to define concrete parsers. This class
        contains helpers to call appropriate parsing functions based on a parsing map,
        which is a mapping of strings to `command_info` structures.

        Subclasses should initialize the parsing map pointing to its own methods, eg:

          struct concrete_parser : parser_base<concrete_parser>
          {
              concrete_parser()
              {
                  parsing_table = {
                      {"MY_TOKEN",    {1, &concrete_parser::parse_my_token}},
                      {"OTHER_TOKEN", {1, &concrete_parser::parse_other_token}},
                  };
              }
              
              void
              parse_my_token(const input_it& it)
              {
                  cout << "my token!" << endl;
              }
              
              input_it
              parse_other_token(const input_it& begin, const input_it& end)
              {
                  cout << "other token!" << endl;
                  return begin+1;
              }
          }

        The subclass can then be used to parse a stream of tokens:

          concrete_parser p;
          auto last_parsed = p.parse_loop(token_list.begin(), token_list.end());
    **/
    template<typename T>
    struct parser_base
    {
        using input_it = std::vector<std::string>::iterator;
        /**
            Parse functions are methods of child classes, can have one of two types:
            
             - point_it_fun: takes a single iterator, for parse functions that
                look at either one token ahead, or don't look at any tokens. The
                parsing iterator is always advanced by one position after a parse
                function of this type is called.
        **/
        using point_it_fun = std::function<void(T*, const input_it&)>;
        /**
             - range_it_function: takes a pair of iterators (begin, end) corresponding
                 to the current position of the parsing iterator and the end of the
                 stream of tokens. For parse functions that look at more than one
                 token ahead in the input stream. Should return an iterator in the
                 input stream corresponding to the position where parsing should
                 continue, eg. one after the last token that was successfully parsed
                 by this function.
        **/
        using range_it_fun = std::function<input_it(T*,
                                                    const input_it&,
                                                    const input_it&)>;
        using parse_function_types = xtl::variant<point_it_fun,
                                                  range_it_fun>;

        struct command_info {
            /**
                Minimum number of tokens that should exist in the stream after this
                command is seen.
            **/
            int number_required_arguments;
            /** Parsing function to call when this command is seen **/
            parse_function_types parse_function;
        };

        using free_fun = std::function<void()>;

        std::map<std::string, command_info> mapping_table;

        /** Switch implementation with strings **/
        bool simple_switch(const std::string& token,
                           std::map<std::string,
                           free_fun> handlers)
        {
            auto handler_it = handlers.find(to_upper(token));
            if (handler_it == handlers.end()) {
                return false;
            }
            /** Call the handler **/
            handler_it->second();
            return true;
        }

        /**
            Override this function to be able to parse any initial tokens before the
            parsing table takes over processing of tokens. This can be used for cases
            where a command has one or more required attributes before all optional
            attributes are handled.
        **/
        input_it parse_init(const input_it& begin, const input_it& end)
        {
            return begin;
        }

        /** Parse a single token (and its dependencies)
           Returns an iterator past the last successfully parsed token
        **/
        input_it parse_step(const input_it& begin, const input_it& end)
        {
            input_it it = begin;
            std::cout << "🦋 parsing: " << *it << std::endl;

            auto cmd_it = mapping_table.find(to_upper(*it));
            if (cmd_it == mapping_table.end())
            {
                return it; 
            }

            command_info cmd_info = cmd_it->second;

            /** Prevents code to end prematurely **/
            if (std::distance(it, end) < cmd_info.number_required_arguments)
            {
                throw std::runtime_error("Arguments missing.");
            }

            /** Advances to next token **/
            ++it;

            /** Calls parsing function for command **/
            if (cmd_info.parse_function.index() == 0)
            {
                /** Calls command functions that receive a point iterator **/
                xtl::get<0>(cmd_info.parse_function)(static_cast<T*>(this), it);
                it++;
            }
            else if (cmd_info.parse_function.index() == 1)
            {
                /** Calls command functions that receive a range iterator **/
                it = xtl::get<1>(cmd_info.parse_function)(static_cast<T*>(this),
                                                          it,
                                                          end);
            }
            return it;
        }

        /**
            Parse all tokens in the range of iterators passed
            Returns an iterator past the last successfully parsed token
        **/
        input_it parse_loop(const input_it& begin, const input_it& end)
        {
            /** First handle initial tokens **/
            input_it it = static_cast<T*>(this)->parse_init(begin, end);

            /** Then move on to the mapping table **/
            while (it != end)
            {
                input_it next = parse_step(it, end);

                if (next == it)
                {
                    break;
                }

                it = next;
            }

            return it;
        }

        /* Implementation of a visitor for xv::xany which is a xtl::any */
        using visitor_map_type = std::unordered_map<std::type_index, std::function<void(xtl::any const&)>>;

        template<typename U, typename F>
        inline std::pair<const std::type_index, std::function<void(xtl::any const&)> >
            to_any_visitor(F const &f)
        {
            return {
                std::type_index(typeid(U)),
                [=](xtl::any const &a)
                {
                    f(xtl::any_cast<U const&>(a));
                }
            };
        }

        inline void visit_any(const xtl::any& a, const visitor_map_type& any_visitor)
        {
            if (const auto it = any_visitor.find(std::type_index(a.type()));
                it != any_visitor.cend()) {
                it->second(a);
            } else {
                std::cout << "Unregistered type "<< std::quoted(demangle(a.type().name()));
            }
        }

        template<typename U, typename F>
        inline void register_any_visitor(F const& f, visitor_map_type& any_visitor)
        {
            std::cout << "Register visitor for type "
                      << std::quoted(typeid(U).name()) << '\n';
            any_visitor.insert(to_any_visitor<U>(f));
        }
    };

    //TODO: I don't think most final value() calls are necessary

    struct bin_parser : parser_base<bin_parser>
    {
        xv::Bin& bin;
        int num_parsed_attrs = 0;

        bin_parser(xv::Bin& bin) : bin(bin)
        {
            //TODO: implemente DIVIDE, EXTENT, STEPS
            mapping_table = {
                {"ANCHOR",  { 1, &bin_parser::parse_bin_anchor  }},
                {"BASE",    { 1, &bin_parser::parse_bin_base    }},
                {"BINNED",  { 1, &bin_parser::parse_bin_binned  }},
                // {"DIVIDE",  { 1, &bin_parser::parse_bin_divide  }},
                // {"EXTENT",  { 1, &bin_parser::parse_bin_extent  }},
                {"MAXBINS", { 1, &bin_parser::parse_bin_maxbins }},
                {"MINSTEP", { 1, &bin_parser::parse_bin_minstep }},
                {"NICE",    { 1, &bin_parser::parse_bin_nice    }},
                {"STEP",    { 1, &bin_parser::parse_bin_step    }},
                // {"STEPS",   { 1, &bin_parser::parse_bin_steps   }},
            };
        }

        void parse_bin_anchor(const input_it& it)
        {
            bin.anchor().value() = std::stod(*it);
            num_parsed_attrs++;
        }

        void parse_bin_base(const input_it& it)
        {
            bin.base().value() = std::stod(*it);
            num_parsed_attrs++;
        }

        void parse_bin_binned(const input_it& it)
        {
            simple_switch(*it,
            {
                {"TRUE",  [&]{ bin.binned().value() = true;
                    num_parsed_attrs++; }},
                {"FALSE", [&]{ bin.binned().value() = false;
                    num_parsed_attrs++; }},
            });
        }

        void parse_bin_maxbins(const input_it& it)
        {
            bin.maxbins().value() = std::stod(*it);
            num_parsed_attrs++;
        }

        void parse_bin_minstep(const input_it& it)
        {
            bin.minstep().value() = std::stod(*it);
            num_parsed_attrs++;
        }

        void parse_bin_nice(const input_it& it)
        {
            simple_switch(*it,
            {
                {"TRUE",  [&]{ bin.nice().value() = true;
                    num_parsed_attrs++; }},
                {"FALSE", [&]{ bin.nice().value() = false;
                    num_parsed_attrs++; }},
            });
        }

        void parse_bin_step(const input_it& it)
        {
            bin.step().value() = std::stod(*it);
            num_parsed_attrs++;
        }
    };

    struct field_parser : parser_base<field_parser>
    {
        using xy_variant = xtl::variant<xv::X*, xv::Y*>;
        xy_variant enc;

        field_parser(xy_variant enc) : enc(enc)
        {
            mapping_table = {
                {"TYPE",      { 1, &field_parser::parse_field_type      }},
                {"BIN",       { 1, &field_parser::parse_field_bin       }},
                {"AGGREGATE", { 1, &field_parser::parse_field_aggregate }},
                {"TIME_UNIT", { 1, &field_parser::parse_field_time_unit }},
            };
        }

        input_it parse_init(const input_it& begin, const input_it&)
        {
            xtl::visit([&](auto &&x_or_y)
            {
                x_or_y->field = *begin;
                x_or_y->type = "quantitative";
            }, enc);

            return begin + 1;
        }

        input_it parse_field_bin(const input_it& begin, const input_it& end)
        {
            bool found = xtl::visit([&](auto &&x_or_y)
            {
                return simple_switch(*begin,
                {
                    {"TRUE",  [&]{ x_or_y->bin().value() = true;  }},
                    {"FALSE", [&]{ x_or_y->bin().value() = false; }},
                });
            }, enc);

            if (found)
            {
                return begin + 1;
            }
            else
            {
                xv::Bin bin;
                bin_parser parser(bin);
                input_it it = parser.parse_loop(begin, end);

                if (parser.num_parsed_attrs == 0)
                {
                    throw std::runtime_error("Missing or invalid BIN type");
                }

                xtl::visit([&](auto &&x_or_y)
                {
                    x_or_y->bin().value() = bin;
                }, enc);

                return it;
            }

        }

        void parse_field_type(const input_it& it)
        {
            bool found = xtl::visit([&](auto &&x_or_y)
            {
                return simple_switch(*it,
                {
                    {"QUANTITATIVE", [&]{ x_or_y->type().value() = "quantitative"; }},
                    {"NOMINAL",      [&]{ x_or_y->type().value() = "nominal";      }},
                    {"ORDINAL",      [&]{ x_or_y->type().value() = "ordinal";      }},
                    {"TEMPORAL",     [&]{ x_or_y->type().value() = "temporal";     }},
                });
            }, enc);
            if (!found)
            {
                throw std::runtime_error("Missing or invalid TYPE type");
            }
        }

        input_it parse_field_aggregate(const input_it& begin, const input_it&)
        {
            bool found = xtl::visit([&](auto &&x_or_y)
            {
                return simple_switch(*begin,
                {
                    {"COUNT",     [&]{ x_or_y->aggregate().value() = "count";     }},
                    {"VALID",     [&]{ x_or_y->aggregate().value() = "valid";     }},
                    {"MISSING",   [&]{ x_or_y->aggregate().value() = "missing";   }},
                    {"DISTINCT",  [&]{ x_or_y->aggregate().value() = "distinct";  }},
                    {"SUM",       [&]{ x_or_y->aggregate().value() = "sum";       }},
                    {"PRODUCT",   [&]{ x_or_y->aggregate().value() = "product";   }},
                    {"MEAN",      [&]{ x_or_y->aggregate().value() = "mean";      }},
                    {"AVERAGE",   [&]{ x_or_y->aggregate().value() = "average";   }},
                    {"VARIANCE",  [&]{ x_or_y->aggregate().value() = "variance";  }},
                    {"VARIANCEP", [&]{ x_or_y->aggregate().value() = "variancep"; }},
                    {"STDEV",     [&]{ x_or_y->aggregate().value() = "stdev";     }},
                    {"STEDEVP",   [&]{ x_or_y->aggregate().value() = "stedevp";   }},
                    {"STEDERR",   [&]{ x_or_y->aggregate().value() = "stederr";   }},
                    {"MEDIAN",    [&]{ x_or_y->aggregate().value() = "median";    }},
                    {"Q1",        [&]{ x_or_y->aggregate().value() = "q1";        }},
                    {"Q3",        [&]{ x_or_y->aggregate().value() = "q3";        }},
                    {"CI0",       [&]{ x_or_y->aggregate().value() = "ci0";       }},
                    {"CI1",       [&]{ x_or_y->aggregate().value() = "ci1";       }},
                    {"MIN",       [&]{ x_or_y->aggregate().value() = "min";       }},
                    {"MAX",       [&]{ x_or_y->aggregate().value() = "max";       }},
                    {"ARGMIN",    [&]{ x_or_y->aggregate().value() = "argmin";    }},
                    {"ARGMAX",    [&]{ x_or_y->aggregate().value() = "argmax";    }},
                    //TODO: missing values arg
                });
            }, enc);
            if (!found)
            {
                throw std::runtime_error("Missing or invalid AGGREGATE type");
            }
            return begin + 1;
        }

        void parse_field_time_unit(const input_it& it)
        {
            bool found = xtl::visit([&](auto &&x_or_y)
            {
                return simple_switch(*it,
                {
                    {"YEAR",        [&]{ x_or_y->timeUnit().value() = "year";        }},
                    {"QUARTER",     [&]{ x_or_y->timeUnit().value() = "quarter";     }},
                    {"MONTH",       [&]{ x_or_y->timeUnit().value() = "month";       }},
                    {"DAY",         [&]{ x_or_y->timeUnit().value() = "day";         }},
                    {"DATE",        [&]{ x_or_y->timeUnit().value() = "date";        }},
                    {"HOURS",       [&]{ x_or_y->timeUnit().value() = "hours";       }},
                    {"MINUTES",     [&]{ x_or_y->timeUnit().value() = "minutes";     }},
                    {"SECONDS",     [&]{ x_or_y->timeUnit().value() = "seconds";     }},
                    {"MILISECONDS", [&]{ x_or_y->timeUnit().value() = "miliseconds"; }},
                });
            }, enc);
            if (!found)
            {
                throw std::runtime_error("Missing or invalid TIME_UNIT type");
            }
        }
    };

    struct mark_parser : parser_base<mark_parser>
    {
        xv::Chart& chart;

        mark_parser(xv::Chart& chart) : chart(chart)
        {
            mapping_table = {
                {"COLOR", { 1, &mark_parser::parse_color }},
            };
        }

        input_it parse_init(const input_it& begin, const input_it&)
        {
            bool found = simple_switch(*begin,
            {
                {"ARC",    [&]{ this->chart.mark() = xv::mark_arc();    }},
                {"AREA",   [&]{ this->chart.mark() = xv::mark_area();   }},
                {"BAR",    [&]{ this->chart.mark() = xv::mark_bar();    }},
                {"CIRCLE", [&]{ this->chart.mark() = xv::mark_circle(); }},
                {"LINE",   [&]{ this->chart.mark() = xv::mark_line();   }},
                {"POINT",  [&]{ this->chart.mark() = xv::mark_point();  }},
                {"RECT",   [&]{ this->chart.mark() = xv::mark_rect();   }},
                {"RULE",   [&]{ this->chart.mark() = xv::mark_rule();   }},
                {"SQUARE", [&]{ this->chart.mark() = xv::mark_square(); }},
                {"TICK",   [&]{ this->chart.mark() = xv::mark_tick();   }},
                {"TRAIL",  [&]{ this->chart.mark() = xv::mark_trail();  }},
            });
            if (!found)
            {
                throw std::runtime_error("Missing or invalid MARK type");
            }
            return begin + 1;
        }

        void parse_color(const input_it& it)
        {
            visitor_map_type any_visitor {
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_arc&>>    ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_area&>>   ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_bar&>>    ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_circle&>> ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_line&>>   ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_point&>>  ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_rect&>>   ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_rule&>>   ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_square&>> ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_tick&>>   ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
                to_any_visitor<xtl::xclosure_wrapper<xv::mark_trail&>>  ([&](auto mark_generic) { mark_generic.get().color = to_lower(*it); }),
            };

            visit_any(this->chart.mark().value(), any_visitor);
        }
    };

    struct xv_sqlite_parser : parser_base<xv_sqlite_parser>
    {
        xv::Chart& chart;

        xv_sqlite_parser(xv::Chart& chart) : chart(chart)
        {
            mapping_table = {
                {"WIDTH",   { 1, &xv_sqlite_parser::parse_width   }},
                {"HEIGHT",  { 1, &xv_sqlite_parser::parse_height  }},
                {"X_FIELD", { 1, &xv_sqlite_parser::parse_x_field }},
                {"Y_FIELD", { 1, &xv_sqlite_parser::parse_y_field }},
                {"MARK",    { 1, &xv_sqlite_parser::parse_mark    }},
                {"GRID",    { 1, &xv_sqlite_parser::parse_grid    }},
                {"TITLE",   { 1, &xv_sqlite_parser::parse_title   }},
            };
        }

        input_it parse_init(const input_it& begin, const input_it&)
        {
            auto ac = xv::axis_config().grid(true);
            auto cf = xv::Config().axis(ac);
            this->chart.config() = cf;
            return begin;
        }

        void parse_width(const input_it& it)
        {
            this->chart.width() = std::stoi(*it);
        }

        void parse_height(const input_it& it)
        {
            this->chart.height() = std::stoi(*it);
        }

        input_it parse_x_field(const input_it& begin, const input_it& end)
        {
            xv::X x_enc = xv::X();
            this->chart.encoding().value().x = x_enc;

            field_parser parser(&this->chart.encoding().value().x().value());
            return parser.parse_loop(begin, end);
        }

        input_it parse_y_field(const input_it& begin, const input_it& end)
        {
            xv::Y y_enc = xv::Y();
            this->chart.encoding().value().y = y_enc;

            field_parser parser(&this->chart.encoding().value().y().value());
            return parser.parse_loop(begin, end);
        }

        input_it parse_mark(const input_it& input, const input_it& end)
        {
            mark_parser parser(this->chart);
            return parser.parse_loop(input, end);
        }

        void parse_grid(const input_it& it)
        {
            bool found = simple_switch(*it,
                {
                    {"TRUE",  [&]{ this->chart.config().value().axis().value().grid() = true;  }},
                    {"FALSE", [&]{ this->chart.config().value().axis().value().grid() = false; }},
                });
            if (!found)
            {
                throw std::runtime_error("Missing or invalid GRID type");
            }
        }

        //TODO: not working
        void parse_title(const input_it& it)
        {
            std::vector<std::string> v = {*it};
            // this->chart.title().value() = v;
        }
    };

    static nl::json process_xvega_input(std::vector<std::string>
                                               tokenized_input,
                                               xv::df_type xv_sqlite_df)
    {
        /** Initializes and populates xeus_sqlite object **/
        xv::Chart chart;
        chart.encoding() = xv::Encodings();

        /** Populates chart with data gathered on interpreter::process_SQLite_input **/
        xv::data_frame data_frame;
        data_frame.values = xv_sqlite_df;
        chart.data() = data_frame;

        /** Parse XVEGA_PLOT syntax **/
        xv_sqlite_parser parser(chart);
        auto last_parsed = parser.parse_loop(tokenized_input.begin(),
                                             tokenized_input.end());
        if (last_parsed != tokenized_input.end())
        {
            throw std::runtime_error("This is not a valid command for SQLite XVega.");
        }

        return xv::mime_bundle_repr(chart);
    }
}

#endif

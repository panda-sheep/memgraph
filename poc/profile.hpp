#pragma once

#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <strings.h>
#include <unistd.h>
#include <unordered_map>

#include "barrier/barrier.hpp"

using namespace std;

namespace barrier
{

// TODO: Turn next template, expand on it, standardize it, and use it for query
// generation.

template <class C>
void fill_to_fill(EdgeAccessor &e, const EdgeType &type, C &&consumer)
{
    if (e.fill() && e.edge_type() == type) {
        auto to = e.to();
        if (to.fill()) {
            consumer(to);
        }
    }
}

template <class C>
void fill_from_fill(EdgeAccessor &e, const EdgeType &type, C &&consumer)
{
    if (e.fill() && e.edge_type() == type) {
        auto from = e.from();
        if (from.fill()) {
            consumer(from);
        }
    }
}

template <class C>
void fill_to_fill(EdgeAccessor &e, C &&consumer)
{
    if (e.fill()) {
        auto to = e.to();
        if (to.fill()) {
            consumer(to);
        }
    }
}

template <class C>
void to_fill(EdgeAccessor &e, C &&consumer)
{
    auto to = e.to();
    if (to.fill()) {
        consumer(to);
    }
}

template <class C>
void to_fill(EdgeAccessor &e, const Label &label, C &&consumer)
{
    auto to = e.to();
    if (to.fill() && to.has_label(label)) {
        consumer(to);
    }
}

template <class C>
void to_fill(EdgeAccessor &e, const EdgeType &type, const Label &label,
             C &&consumer)
{
    if (e.edge_type() == type) {
        auto to = e.to();
        if (to.fill() && to.has_label(label)) {
            consumer(to);
        }
    }
}

template <class C>
void from_fill(EdgeAccessor &e, const EdgeType &type, C &&consumer)
{
    if (e.edge_type() == type) {
        auto from = e.from();
        if (from.fill()) {
            consumer(from);
        }
    }
}

template <class C>
void fill_from_fill(EdgeAccessor &e, C &&consumer)
{
    if (e.fill()) {
        auto from = e.from();
        if (from.fill()) {
            consumer(from);
        }
    }
}

namespace iter
{
template <class I, class C>
void for_all_fill(I iter, C &&consumer)
{
    auto e = iter.next();
    while (e.is_present()) {
        if (e.get().fill()) consumer(e.take());
        e = iter.next();
    }
}

template <class I, class C>
void find(I iter, C &&consumer)
{
    auto e = iter.next();
    while (e.is_present()) {

        if (consumer(e.take())) {
            return;
        }
        e = iter.next();
    }
}

template <class I, class C>
void find_fill(I iter, C &&consumer)
{
    auto e = iter.next();
    while (e.is_present()) {
        if (e.get().fill()) {
            if (consumer(e.take())) {
                return;
            }
        }
        e = iter.next();
    }
}
}

void fill_with_bt(unordered_map<string, double> &values, VertexAccessor &com,
                  double weight,
                  VertexPropertyType<ArrayString> &prop_vertex_business_types)
{
    auto bus_t = com.at(prop_vertex_business_types);
    if (bus_t.is_present()) {
        for (auto &bt : bus_t.get()->value) {
            values[bt] += weight;
        }
    }
}

void oportunity_employe_company(
    VertexAccessor &va, unordered_map<string, double> &values, double weight,
    VertexPropertyType<ArrayString> &prop_vertex_business_types,
    const EdgeType &type_created, const EdgeType &type_works_in,
    const Label &label_company)
{
    iter::for_all_fill(va.in(), [&](auto opp_e) {
        // cout << "                       oec.in()" << endl;
        from_fill(opp_e, type_created, [&](auto creator) {
            // cout << "                           type_created" << endl;
            iter::for_all_fill(creator.out(), [&](auto creator_e) {
                // cout << "                               creator.out()" <<
                // endl;
                to_fill(creator_e, type_works_in, label_company,
                        [&](auto end_com) {
                            // cout << " fill_bt"
                            //      << endl;
                            fill_with_bt(values, end_com, weight,
                                         prop_vertex_business_types);
                        });
            });

        });
    });
}

auto query(DbAccessor &t, const Id &start_id)
{
    // DbAccessor t(db);
    unordered_map<string, double> values;

    const Label &label_company = t.label_find_or_create("Company");
    const Label &label_opportunuty = t.label_find_or_create("Opportunity");

    const EdgeType &type_works_in = t.type_find_or_create("Works_In");
    const EdgeType &type_reached_to = t.type_find_or_create("Reached_To");
    const EdgeType &type_partnered_with =
        t.type_find_or_create("Partnered_With");
    const EdgeType &type_interested_in = t.type_find_or_create("Interested_In");
    const EdgeType &type_viewed = t.type_find_or_create("Viewed");
    const EdgeType &type_has_match = t.type_find_or_create("Has_Match");
    const EdgeType &type_searched_and_clicked =
        t.type_find_or_create("Searched_And_Clicked");
    const EdgeType &type_is_employee = t.type_find_or_create("Is_Employee");
    const EdgeType &type_created = t.type_find_or_create("Created");

    auto prop_edge_status = t.edge_property_key<String>("status");
    auto prop_edge_count = t.edge_property_key<Int32>("count");
    auto prop_edge_feedback = t.edge_property_key<String>("feedback");

    auto prop_vertex_business_types =
        t.vertex_property_key<ArrayString>("business_types");

    auto osva = t.vertex_find(start_id);
    if (!option_fill(osva)) {
        cout << "Illegal start vertex" << endl;
        return values;
    }
    auto start = osva.take();

    // PARTNERS
    iter::for_all_fill(start.out(), [&](auto e) {
        // cout << "start.out()" << endl;
        to_fill(e, type_partnered_with, label_company, [&](auto end_com) {
            fill_with_bt(values, end_com, 0.9, prop_vertex_business_types);
        });
    });

    // PERSONELS
    ::iter::for_all(start.in(), [&](auto e) {
        // cout << "start.in()" << endl;
        fill_from_fill(e, type_works_in, [&](auto employ) {
            // cout << "   type_works_in" << endl;
            iter::for_all_fill(employ.out(), [&](auto employ_edge) {
                // cout << "       employ.out()" << endl;
                auto &ee_type = employ_edge.edge_type();
                // cout << "       ee_type: " << ee_type << endl;

                if (ee_type == type_interested_in) {
                    // cout << "           type_interested_in" << endl;
                    // INTERESTED IN OPPORTUNUTIES
                    to_fill(employ_edge, label_opportunuty, [&](auto opp) {
                        oportunity_employe_company(
                            opp, values, 1, prop_vertex_business_types,
                            type_created, type_works_in, label_company);

                    });

                } else if (ee_type == type_created) {
                    // cout << "           type_created" << endl;
                    // CREATED OPPORTUNUTIES
                    to_fill(employ_edge, label_opportunuty, [&](auto opp) {
                        iter::for_all_fill(opp.out(), [&](auto edge) {
                            auto feedback = edge.at(prop_edge_feedback);
                            if (!feedback.is_present()) {
                                return;
                            }

                            auto str = feedback.get()->value.c_str();
                            double weight = 0;
                            if (strcasecmp(str, "like") == 0) {
                                weight = 1;
                            } else if (strcasecmp(str, "dislike") == 0) {
                                weight = -1;
                            } else {
                                return;
                            }

                            to_fill(edge, label_company, [&](auto end_com) {
                                fill_with_bt(values, end_com, weight,
                                             prop_vertex_business_types);
                            });
                        });
                    });

                } else {
                    // cout << "           company" << endl;
                    // COMPANY
                    double weight = 0;
                    if (ee_type == type_reached_to) {
                        auto os = employ_edge.at(prop_edge_status);
                        if (!os.is_present()) {
                            return;
                        }
                        auto str = os.get()->value.c_str();

                        if (strcasecmp(str, "pending") == 0) {
                            weight = 0.5;
                        } else if (strcasecmp(str, "connected") == 0) {
                            weight = 1;
                        } else if (strcasecmp(str, "unreachable") == 0) {
                            weight = 0.5;
                        } else if (strcasecmp(str, "not_a_match") == 0) {
                            weight = -1;
                        } else {
                            cout << "unknown status: " << str << endl;
                        }
                    } else if (ee_type == type_viewed ||
                               ee_type == type_searched_and_clicked) {
                        auto count = employ_edge.at(prop_edge_count);
                        if (count.is_present()) {
                            weight = 0.01 * (count.get()->value);
                        }
                    }

                    // TARGET COMPANY
                    if (weight != 0) {
                        to_fill(employ_edge, [&](auto t_com) {
                            fill_with_bt(values, t_com, weight,
                                         prop_vertex_business_types);
                        });
                    }
                }
            });
        });
    });

    return values;
}

Option<Id> find_company(DbAccessor &t, int64_t cid)
{
    // DbAccessor t(db);

    Option<Id> found;

    auto prop_vertex_company_id = t.vertex_property_key<Int64>("company_id");
    const Label &label_company = t.label_find_or_create("Company");

    iter::find_fill(label_company.index().for_range(t), [&](auto v) {
        if (v.has_label(label_company)) {
            auto id = v.at(prop_vertex_company_id);
            if (id.is_present()) {
                if ((*id.get()) == cid) {
                    found = Option<Id>(v.id());
                    return true;
                }
            }
        }
        return false;
    });

    return found;
}

size_t for_all_companys(
    DbAccessor &t,
    vector<pair<VertexAccessor, unordered_map<string, double>>> &coll)
{
    int i = 0;
    iter::for_all_fill(
        t.label_find_or_create("Company").index().for_range(t), [&](auto v) {
            coll.push_back(make_pair(v, barrier::query(t, v.id())));
            i++;
            return false;
        });
    return i;
}
}

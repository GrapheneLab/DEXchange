#include <dexchange/dexchange.hpp>
#include <eosio/system.hpp>

using eosio::current_time_point;

bool globalstate::token_permitted(const asset& a) const {
    return  permitted_tokens.find(a.symbol) != permitted_tokens.end();
}

std::optional<Pair_info> globalstate::pair_permitted(const asset& a, const asset& b) const {
    
    for(auto it = permitted_pairs.begin(); it != permitted_pairs.end(); it++)
    {
        if((it->sell == a.symbol && it->buy == b.symbol) ||
           (it->sell == b.symbol && it->buy == a.symbol))
            return *it;
    }

    return std::optional<Pair_info>();
 }

const bool operator < (const Order& a, const Order& b) {
    if(a.price < b.price) {
        return true;
    }
    else if (a.price > b.price) {
        return false;
    }
    else {
        if(a.by_time() < b.by_time())
            return true;
        else if(a.by_time() > b.by_time())
            return false;
        else if(a.by_value() > b.by_value())
            return true;
        else if(a.by_value() < b.by_value())
            return true;
        else if(a.total_id < b.total_id)
            return true;
        else
            return false;
    }
}

void Order::update_average_price(const asset& r, const asset& p, const asset& f, bool convert) { 
    received += r;
    paid += p;
    check(sell >= paid, "error sell < paid");
    average_price = (received.amount / pow(10,received.symbol.precision())) / (paid.amount / pow(10, paid.symbol.precision()));
    if(convert)
        average_price = 1/average_price;
    received -= f;
    fee += f;
}

uint64_t dexchange::get_new_total_order_id() {
    uint64_t id = gstate.total_order_id++;
    global.set(gstate, _self);
    return id;
}

void Orders::insert_order(Order& o) {
    if(sell == o.sell.symbol) {
        sell_orders.push_front(o);
        sell_orders.sort();
    }
    else {
        buy_orders.push_front(o);
        buy_orders.sort();
    }
}

Order dexchange::init_order(    const name&    owner,
                                const asset&   sell,
                                const asset&   buy,
                                const symbol&  sell_symbol) {
    Order o;
    o.total_id = get_new_total_order_id();
    o.owner = owner;
    o.start_time = current_time_point();
    o.sell = sell;
    o.buy = buy;
    o.received = buy;
    o.received.amount = 0;
    o.paid = sell;
    o.paid.amount = 0;
    o.fee = o.received;

    if(sell_symbol == o.sell.symbol)
        o.price = (buy.amount / pow(10,buy.symbol.precision())) / (sell.amount / pow(10,sell.symbol.precision()));
    else
        o.price = (sell.amount / pow(10,sell.symbol.precision())) / (buy.amount / pow(10,buy.symbol.precision()));

    o.average_price = o.price;
    return o;
}

void dexchange::order(  const name&    owner,
                        const asset&   sell,
                        const asset&   buy)
{
    require_auth(owner);
    check(blacklist.find(owner.value) == blacklist.end(), "This account has been blacklisted");
    auto itr_owner = accounts.find(owner.value);
    check(itr_owner != accounts.end(), "no owner found");
    auto p = gstate.pair_permitted(sell, buy);
    check(p.has_value(), "pair is not permitted");
    check(sell.amount != 0 && buy.amount != 0, "zero asset not permitted");
    auto itr_balance = itr_owner->balances.find(sell.symbol);
    check(itr_balance != itr_owner->balances.end(), "sell asset not found");
    check(itr_balance->second.available >= sell, "sell asset not enough");

    check(sell >= gstate.fee[sell.symbol].min_order, "the order is less than minimum order");

    accounts.modify(itr_owner, _self, [&] (auto& acnt) {
        acnt.balances[sell.symbol].available -= sell;
        acnt.balances[sell.symbol].used += sell;
    });

    Order o = init_order(owner, sell, buy, p->sell);

    auto itr_all_orders = all_orders.find(sell.symbol.raw()^buy.symbol.raw());
    if(itr_all_orders == all_orders.end())
        itr_all_orders = all_orders.emplace(_self, [&] (auto& table) {
            table.sell = p->sell;
            table.buy = p->buy;
            table.insert_order(o);
        });
    else
        all_orders.modify(itr_all_orders, _self, [&] (auto& table) {
            table.insert_order(o);
        });

    all_orders_info.emplace(_self, [&] (auto& order) {
        order = o;
    });

    matching(itr_all_orders->sell_orders, itr_all_orders->buy_orders);
}

Order get_maker(const Order& a, const Order& b) {

    if (a.start_time < b.start_time)
        return a;
    if (b.start_time < a.start_time)
        return b;
    if (a.total_id < b.total_id)
        return a;
    return b;
}

void dexchange::order_to_history(const Order& o, uint8_t close_status) {

    auto h = orders_history.emplace(_self, [&] (auto& h) {
        h.total_id = o.total_id;
        h.close_status = close_status;
        h.owner = o.owner;
        h.start_time = o.start_time;;
        h.end_time = current_time_point();
        h.sell = o.sell;
        h.buy = o.buy;
        h.received = o.received;
        h.paid = o.paid;
        h.fee = o.fee;
        h.price = o.price;
        h.average_price = o.average_price;
    });

    auto itr_info = all_orders_info.find(o.start_time.elapsed.count() ^ o.total_id);
    all_orders_info.erase(itr_info);    
}

void dexchange::modify_orders_info(Order& o) {
    auto itr_info = all_orders_info.find(o.start_time.elapsed.count() ^ o.total_id);
    all_orders_info.modify(itr_info, _self, [&] (auto& order) {
        order = o;
    }); 
}

void Bucket::update(asset& sell, asset& buy, double price) {
    if(high_base < price)
        high_base = price;
    if(low_base > price)
        low_base = price;
    close_base = price;
    base_volume += (sell.amount / pow(10, sell.symbol.precision()));
    quote_volume += (buy.amount / pow(10, buy.symbol.precision()));
}

void dexchange::update_buckets(asset& sell, asset& buy, double price) {

    Bucket init_bucket;
    init_bucket.base = sell.symbol;
    init_bucket.quote = buy.symbol;
    init_bucket.high_base = price;
    init_bucket.low_base = price;
    init_bucket.open_base = price;
    init_bucket.close_base = price;
    init_bucket.base_volume = sell.amount / pow(10, sell.symbol.precision());
    init_bucket.quote_volume = buy.amount / pow(10, buy.symbol.precision());

    auto bucket_indx1 = buckets1.get_index<"bypairtime"_n>();
    auto bucket_indx2 = buckets2.get_index<"bypairtime"_n>();
    auto bucket_indx3 = buckets3.get_index<"bypairtime"_n>();
    auto bucket_indx4 = buckets4.get_index<"bypairtime"_n>();
    auto bucket_indx5 = buckets5.get_index<"bypairtime"_n>();
    auto bucket_indx6 = buckets6.get_index<"bypairtime"_n>();
    auto bucket_indx7 = buckets7.get_index<"bypairtime"_n>();

    uint64_t cur_time_seconds = current_time_point().sec_since_epoch ();

    for(uint32_t bucket: gstate.buckets) {

        uint64_t bucket_num =  cur_time_seconds / bucket;
        time_point_sec open = time_point_sec() + bucket_num * bucket;
        
        init_bucket.open = open;
        init_bucket.bucket = bucket;

        uint64_t key = sell.symbol.raw()^buy.symbol.raw()^open.sec_since_epoch();

        auto bucket_itr1 = bucket_indx1.find(key);
        auto bucket_itr2 = bucket_indx2.find(key);
        auto bucket_itr3 = bucket_indx3.find(key);
        auto bucket_itr4 = bucket_indx4.find(key);
        auto bucket_itr5 = bucket_indx5.find(key);
        auto bucket_itr6 = bucket_indx6.find(key);
        auto bucket_itr7 = bucket_indx7.find(key);

        switch(bucket) {
            case 60:
                if(bucket_itr1 == bucket_indx1.end()) {
                    buckets1.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets1.available_primary_key();
                    });
                }
                else {
                    bucket_indx1.modify(bucket_itr1, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;
            
            case 300:
                if(bucket_itr2 == bucket_indx2.end()) {
                    buckets2.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets2.available_primary_key();
                    });
                }
                else {
                    bucket_indx2.modify(bucket_itr2, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

            case 900:
                if(bucket_itr3 == bucket_indx3.end()) {
                    buckets3.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets3.available_primary_key();
                    });
                }
                else {
                    bucket_indx3.modify(bucket_itr3, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

            case 1800:
                if(bucket_itr4 == bucket_indx4.end()) {
                    buckets4.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets4.available_primary_key();
                    });
                }
                else {
                    bucket_indx4.modify(bucket_itr4, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

            case 3600:
                if(bucket_itr5 == bucket_indx5.end()) {
                    buckets5.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets5.available_primary_key();
                    });
                }
                else {
                    bucket_indx5.modify(bucket_itr5, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

            case 14400:
                if(bucket_itr6 == bucket_indx6.end()) {
                    buckets6.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets6.available_primary_key();
                    });
                }
                else {
                    bucket_indx6.modify(bucket_itr6, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

            case 86400:
                if(bucket_itr7 == bucket_indx7.end()) {
                    buckets7.emplace( _self, [&] (auto& b) {
                        b = init_bucket;
                        b.id = buckets7.available_primary_key();
                    });
                }
                else {
                    bucket_indx7.modify(bucket_itr7, _self, [&] (auto& b) {
                        b.update(sell, buy, price);
                    });
                }
                break;

        } // switch
    }
}

void dexchange::send_order_tokens(const eosio::name& from, const eosio::name& to, const eosio::asset& quantity, const eosio::asset& fee) {

    auto itr_from = accounts.find(from.value);
    auto itr_balance = itr_from->balances.find(quantity.symbol);
    check(itr_balance->second.available >= quantity, "not enough balance");

    accounts.modify(itr_from, _self, [&] (auto& acnt){
        acnt.balances[quantity.symbol].used -= quantity;
    });

    check(quantity.amount - fee.amount > 0, " error empty order transfer");
    send_transfer(to, quantity - fee, std::string("Fill order"));

    if(fee.amount > 0) {
        check(fee.amount >= 10, "fee too small");
        asset gl_fee = fee * GL_PERCENT / 100;
        asset sig_fee = fee * SIG_PERCENT / 100;
        if(gl_fee + sig_fee < fee)
            gl_fee += fee - gl_fee - sig_fee;

        eosio::print(" gl_fee=", gl_fee);
        eosio::print(" sig_fee=", sig_fee);

        check(gl_fee + sig_fee == fee, " wrong fee asset");

        send_transfer(name(sig_fee_account), sig_fee, std::string("Revenue from exchange"));
        send_transfer(name(gl_fee_account), gl_fee, std::string("Revenue from exchange"));
    }
}

void dexchange::matching(std::list<Order> sell, std::list<Order> buy)
{
    if(sell.size() == 0 || buy.size() == 0){
        eosio::print(" no sell or buy orders");
        return;
    }

    double buy_max = buy.back().price;
    double sell_min = sell.begin()->price;

    uint64_t table_key = sell.begin()->sell.symbol.raw()^sell.begin()->buy.symbol.raw();
    bool empty_buy = false, empty_sell = false;

    for(auto order_buy = buy.begin(); order_buy != buy.end();)
    {
        empty_buy = false;
        if(sell.size() == 0) {
            eosio::print(" no sell orders.");
            break;
        }

        if(buy_max < sell_min) {
            eosio::print(" no common price");
            break;
        }

        if(order_buy->price < sell.begin()->price) {
            eosio::print(" bad price 1.");
            order_buy++;
            continue;
        }

        for(auto order_sell = sell.begin(); order_sell != sell.end();)
        {
             eosio::print(" order_buy_balance=", order_buy->sell - order_buy->paid);
             eosio::print(" order_sell_balance=", order_sell->sell - order_sell->paid);
            // если цена не подходит - переходим к следующему ордеру на покупку
            if(order_buy->price < order_sell->price) {
                eosio::print(" bad price 2.");
                order_buy++;
                break;
            }

            std::pair<std::list<Order>::iterator, std::list<Order>::iterator> pair = std::pair(order_buy, order_sell);

            double buy_fee, sell_fee;
            Order maker_order = get_maker(*order_buy, *order_sell);
            eosio::print(" order_price=", maker_order.price);

            if(order_buy->sell.symbol == maker_order.sell.symbol) {
                buy_fee = gstate.fee[order_buy->buy.symbol].maker_fee;
                sell_fee = gstate.fee[order_sell->buy.symbol].taker_fee;
            }
            else {
                sell_fee = gstate.fee[order_sell->buy.symbol].maker_fee;
                buy_fee = gstate.fee[order_buy->buy.symbol].taker_fee;
            }

            // определяем сколько по этой цене один может купить а другой продать.
            asset order_sell_max = order_sell->sell - order_sell->paid;

            double buy_amount_max = (order_buy->sell.amount - order_buy->paid.amount) * pow(10,order_buy->buy.symbol.precision());
            buy_amount_max /= maker_order.price * pow(10,order_buy->sell.symbol.precision());

            uint64_t cur_deal_value = std::min(order_sell_max.amount, (int64_t)buy_amount_max);

            asset order_buy_asset = asset(cur_deal_value, order_buy->buy.symbol);
            eosio::print(" order_buy=", order_buy_asset);

            double order_sell_amount = cur_deal_value * maker_order.price * pow(10, order_buy->sell.symbol.precision()) / pow(10,order_buy->buy.symbol.precision());
            asset order_sell_asset = asset(ceil(order_sell_amount), order_buy->sell.symbol);

            eosio::print(" order_sell=", order_sell_asset);

            double sell_fee_amount = order_sell_asset.amount * sell_fee / 100;
            double buy_fee_amount = order_buy_asset.amount * buy_fee / 100;
            asset order_sell_fee = eosio::asset(ceil(sell_fee_amount), order_sell_asset.symbol);
            asset order_buy_fee = eosio::asset(ceil(buy_fee_amount), order_buy_asset.symbol);

            eosio::print(" order_sell_fee=",order_sell_fee);
            eosio::print(" order_buy_fee=",order_buy_fee);

            order_buy->update_average_price(order_buy_asset, order_sell_asset, order_buy_fee, true);
            order_sell->update_average_price(order_sell_asset, order_buy_asset, order_sell_fee, false);
    
            send_order_tokens(order_buy->owner, order_sell->owner, order_sell_asset, order_sell_fee);
            send_order_tokens(order_sell->owner, order_buy->owner, order_buy_asset, order_buy_fee);
    
            if(order_sell->sell == order_sell->paid)
            {
                eosio::print(" empty sell.");
                order_to_history(*order_sell, CLOSED_NORMALLY);
                order_sell = sell.erase(order_sell);
                if(order_sell != sell.end())
                    sell_min = order_sell->price;
                empty_sell = true;                
            }
            else modify_orders_info(*order_sell);

            if(order_buy->sell == order_buy->paid)
            {
                eosio::print(" empty buy.");
                order_to_history(*order_buy, CLOSED_NORMALLY);
                order_buy = buy.erase(order_buy);
                empty_buy = true;                
            }
            else modify_orders_info(*order_buy);

            check(empty_sell || empty_buy, "error no empty order");

            if(!empty_sell || !empty_buy) {
                auto order_itr = order_sell;
                std::list<Order>* orders_list = &sell;
                if(!empty_buy) {
                    eosio::print(" have order_buy yet.");
                    order_itr = order_buy;
                    orders_list = &buy;
                }
                else
                    eosio::print(" have order_sell yet.");

                asset order_balance = order_itr->sell - order_itr->paid;

                if(order_balance < gstate.fee[order_itr->sell.symbol].min_order) {
                    eosio::print(" order too small.");
                    order_to_history(*order_itr, CLOSED_BY_MINIMUM_ORDER_SIZE);
                    accounts.modify(accounts.find(order_itr->owner.value), _self, [&](auto& acnt){
                        acnt.balances[order_balance.symbol].used -= order_balance;
                    });
                    send_transfer(order_itr->owner, order_balance, memos[CLOSED_BY_MINIMUM_ORDER_SIZE]);
                    order_itr = orders_list->erase(order_itr);
                    if( !empty_sell ) {
                        order_sell = order_itr;
                        if(order_itr != orders_list->end())
                            sell_min = order_itr->price;
                    }
                    else{
                        order_buy = order_itr;
                        empty_buy = true;
                    }
                }
            }

            empty_sell = false;
            update_buckets(order_sell_asset, order_buy_asset, maker_order.price);

            if(empty_buy)
                break;
        }
    }

    all_orders.modify(all_orders.find(table_key), _self, [&] (auto& table){
            table.sell_orders = sell;
            table.buy_orders =  buy;
    });
}

void dexchange::drop_orders_common(std::map<uint64_t, std::set<Order>>& orders_by_pairs, std::map< name, std::map<symbol, asset>> assets_to_transfer,
                                    const uint16_t reason) {
    
    erase_by_pair_orders(orders_by_pairs, reason);

    for(auto account_itr = assets_to_transfer.begin(); account_itr != assets_to_transfer.end(); account_itr++) {
        accounts.modify(accounts.find(account_itr->first.value), _self, [&](auto& acnt){
            for(auto balance_itr = account_itr->second.begin(); balance_itr != account_itr->second.end(); balance_itr++) {
                acnt.balances[balance_itr->first].used -= balance_itr->second;
            }
        });

        for(auto balance_itr = account_itr->second.begin(); balance_itr != account_itr->second.end(); balance_itr++)
            send_transfer(account_itr->first, balance_itr->second, memos[reason]);
    }
}

void dexchange::insert_assets_to_transfer(const Order& order, std::map<name, std::map<symbol, asset>>& assets_to_transfer) {

    if(assets_to_transfer[order.owner].find(order.sell.symbol) == assets_to_transfer[order.owner].end())
        assets_to_transfer[order.owner][order.sell.symbol] = order.sell - order.paid;
    else
        assets_to_transfer[order.owner][order.sell.symbol] += order.sell - order.paid;
}

void dexchange::droporders(const name& owner, std::vector<uint64_t> orders_ids) {
    require_auth(owner);
    check(blacklist.find(owner.value) == blacklist.end(), "This account has been blacklisted");
    auto account_itr = accounts.find(owner.value);
    check(account_itr != accounts.end(), "no owner found");

    std::map<uint64_t, std::set<Order>> orders_by_pairs;
    std::map< name, std::map<symbol, asset>> assets_to_transfer;
    auto id_index = all_orders_info.get_index<"byid"_n>();

    for(uint64_t id: orders_ids) {
        auto order_itr = id_index.find(id);
        if(order_itr != id_index.end() && order_itr->owner == owner) {
            orders_by_pairs[order_itr->sell.symbol.raw()^order_itr->buy.symbol.raw()].insert(*order_itr);
            insert_assets_to_transfer(*order_itr, assets_to_transfer);
        }
    }

    drop_orders_common(orders_by_pairs, assets_to_transfer, CLOSED_BY_USER);
}

void dexchange::dropsmallorders(const symbol& s) {  
    
    std::map<uint64_t, std::set<Order>> orders_by_pairs;
    std::map<name, std::map<symbol,asset>> assets_to_transfer;

    auto size_index = all_orders_info.get_index<"byordersize"_n>();
    auto order_itr = size_index.begin();

    eosio::print(" order_min=", gstate.fee[s].min_order);
    
    for(order_itr = size_index.begin(); order_itr != size_index.end(); order_itr++) {
        if(order_itr->sell.symbol != s)
            continue;
        eosio::print(" size = ", order_itr->sell - order_itr->paid);
        
        if((order_itr->sell - order_itr->paid) >= gstate.fee[s].min_order)
            break;

        if(order_itr->sell.symbol != s)
            continue;

        orders_by_pairs[order_itr->sell.symbol.raw()^order_itr->buy.symbol.raw()].insert(*order_itr);

        insert_assets_to_transfer(*order_itr, assets_to_transfer);
    }

    drop_orders_common(orders_by_pairs, assets_to_transfer, CLOSED_BY_MINIMUM_ORDER_SIZE);
}

void dexchange::dropall(const name& owner) {
    require_auth(owner);
    check(blacklist.find(owner.value) == blacklist.end(), "This account has been blacklisted");
    auto account_itr = accounts.find(owner.value);
    check(account_itr != accounts.end(), "no owner found");

    std::map<uint64_t, std::set<Order>> orders_by_pairs;
    std::map<name, std::map<symbol, asset>> assets_to_transfer;
    auto owner_index = all_orders_info.get_index<"byowner"_n>();
    auto order_itr = owner_index.lower_bound(account_itr->key);

    while(order_itr->owner == owner) {
        orders_by_pairs[order_itr->sell.symbol.raw()^order_itr->buy.symbol.raw()].insert(*order_itr);
        insert_assets_to_transfer(*order_itr, assets_to_transfer);
        order_itr++;
    }

    drop_orders_common(orders_by_pairs, assets_to_transfer, CLOSED_BY_USER);
}

void dexchange::init() {
    require_auth(_self);
    global.set(gstate, _self);
}

void dexchange::send_transfer(const name& to, const asset& quantity, const std::string& memo) {
    action{
        permission_level{_self, "active"_n},
        eosio::name(gstate.permitted_tokens[quantity.symbol]),
        "transfer"_n,                
        std::make_tuple( _self, to, quantity, memo)
    }.send();
}

void dexchange::erase_all_pair_orders(const Orders& orders, const uint16_t reason) {

    std::map<name, std::pair<asset, asset>> to_transfer;

    for(auto sell_itr = orders.sell_orders.begin(); sell_itr != orders.sell_orders.end(); sell_itr++) {
        order_to_history(*sell_itr, reason);
                
        if(to_transfer.find(sell_itr->owner) == to_transfer.end()) {
            to_transfer[sell_itr->owner].first = sell_itr->sell - sell_itr->paid;
            to_transfer[sell_itr->owner].second = asset(0, orders.buy);
        }
        else
            to_transfer[sell_itr->owner].first += sell_itr->sell - sell_itr->paid;
    }

    for(auto buy_itr = orders.buy_orders.begin(); buy_itr != orders.buy_orders.end(); buy_itr++) {
        order_to_history(*buy_itr, reason);
                
        if(to_transfer.find(buy_itr->owner) == to_transfer.end()) {
            to_transfer[buy_itr->owner].second = buy_itr->sell - buy_itr->paid;
            to_transfer[buy_itr->owner].first = asset(0, orders.sell);
        }
        else
            to_transfer[buy_itr->owner].second += buy_itr->sell - buy_itr->paid;
    }

    for(auto to_transfer_itr = to_transfer.begin(); to_transfer_itr != to_transfer.end(); to_transfer_itr++) {
        accounts.modify(accounts.find(to_transfer_itr->first.value), _self, [&] (auto& acnt){
            if(to_transfer_itr->second.first.amount != 0)
                acnt.balances[to_transfer_itr->second.first.symbol].used -= to_transfer_itr->second.first;
            if(to_transfer_itr->second.second.amount != 0)
                acnt.balances[to_transfer_itr->second.second.symbol].used -= to_transfer_itr->second.second;
        });
        if(to_transfer_itr->second.first.amount != 0)
            send_transfer(to_transfer_itr->first, to_transfer_itr->second.first, memos[reason]);
        if(to_transfer_itr->second.second.amount != 0)
            send_transfer(to_transfer_itr->first, to_transfer_itr->second.second, memos[reason]);
    }
}

void dexchange::cancel_orders_by_token_pair( const symbol& a, const symbol& b, const uint16_t reason) {

    auto itr_orders = all_orders.find(a.raw()^b.raw());
    erase_all_pair_orders(*itr_orders, reason);
    all_orders.erase(itr_orders);
}

void dexchange::cancel_orders_by_token( const symbol& s, const uint16_t reason) {
    
    for(auto itr_orders = all_orders.begin(); itr_orders != all_orders.end(); ) {

        if(itr_orders->sell != s && itr_orders->buy != s) {
            itr_orders++;
            continue;
        }

        erase_all_pair_orders(*itr_orders, reason);
        itr_orders = all_orders.erase(itr_orders);
    }
}

void dexchange::deltokenpair(const asset& a, const asset& b) {
    require_auth(_self);

    auto pair_it = gstate.permitted_pairs.end();
    for(pair_it = gstate.permitted_pairs.begin(); pair_it != gstate.permitted_pairs.end(); pair_it++)
        if( (pair_it->sell.raw()^pair_it->buy.raw()) == (a.symbol.raw()^b.symbol.raw()))
            break;
    check(pair_it != gstate.permitted_pairs.end(), "assets pair not found");

    cancel_orders_by_token_pair(a.symbol, b.symbol, CLOSED_TOKEN_PAIR_DELETED);

    gstate.permitted_pairs.erase(pair_it);
    global.set(gstate, _self);
}

void dexchange::addtokenpair(const asset& a, const asset& b) {
    require_auth(_self);
    check(a.symbol != b.symbol, "same tokens symbols");
    check(gstate.permitted_tokens.find(a.symbol) != gstate.permitted_tokens.end(), "token not permitted");
    check(gstate.permitted_tokens.find(b.symbol) != gstate.permitted_tokens.end(), "token not permitted");
    auto p = gstate.pair_permitted(a, b);
    check(!p.has_value(), "such a pair already exists");

    Pair_info pair_info{ a.symbol, b.symbol, a.symbol.raw()^b.symbol.raw() };
    gstate.permitted_pairs.push_back(pair_info);
    global.set(gstate, _self);

    for(auto itr = accounts.begin(); itr != accounts.end(); itr++)
        accounts.modify(itr, _self, [&] (auto& acnt){
            acnt.pairs_keys[std::pair(pair_info.sell, pair_info.buy)] = pair_info.key^acnt.key;
        });
}

Fee_info get_fee_info(const symbol& s, const double maker_fee, const double taker_fee) {
    double min_order_amount = 0;
    if(maker_fee != 0 && taker_fee != 0)
        min_order_amount = 100 * MIN_FEE_AMOUNT / std::min(maker_fee, taker_fee);
    asset a = asset(ceil(min_order_amount), s);
    return Fee_info{maker_fee, taker_fee, a};
}

void dexchange::addtoken(const name& contract, const symbol& s, const double maker_fee, const double taker_fee) {
    require_auth(_self);
    
    for(auto it = gstate.permitted_tokens.begin(); it != gstate.permitted_tokens.end(); it++)
        check(it->first.code() != s.code(), "token code exists");

    check( (0 <= maker_fee <= 100.0) && (0 <= taker_fee <= 100.0), "wrong fee");

    gstate.permitted_tokens[s] = contract;
    
    gstate.fee[s] = get_fee_info(s, maker_fee, taker_fee);

    if(gstate.token_contracts.find(contract) != gstate.token_contracts.end())
        gstate.token_contracts[contract].symbols.insert(s);
    else {
        Symbols symbols;
        symbols.symbols.insert(s);
        gstate.token_contracts[contract] = symbols;
    }

    global.set(gstate, _self);
}

void dexchange::return_tokens(const eosio::symbol& s) {

    for(auto acnt_itr = accounts.begin(); acnt_itr != accounts.end(); acnt_itr++) {
        auto token_itr = acnt_itr->balances.find(s);
        if(token_itr == acnt_itr->balances.end())
            continue;

        asset quantity = token_itr->second.available + token_itr->second.used;

        if(quantity.amount != 0)
            send_transfer(acnt_itr->owner, quantity, std::string("This token has been removed from the exchange"));

        accounts.modify(acnt_itr, _self, [&] (auto& acnt){
            acnt.balances.erase(s);
        });
    }
}

void dexchange::deltoken(const name& contract, const symbol& s) {
    require_auth(_self);

    auto it = gstate.permitted_tokens.find(s);
    check(it != gstate.permitted_tokens.end(), "token not found");

    check(gstate.permitted_tokens[s] == contract, "symbol does not match the contract");

    cancel_orders_by_token(s, CLOSED_TOKEN_DELETED);

    return_tokens(s);

    gstate.fee.erase(s);
    gstate.permitted_tokens.erase(it);
    gstate.token_contracts[contract].symbols.erase(s);
    if(gstate.token_contracts[contract].symbols.size() == 0)
        gstate.token_contracts.erase(contract);

    global.set(gstate, _self);
}

void dexchange::setfee(const symbol& s, const double maker_fee, const double taker_fee) {
    require_auth(_self);
    check(gstate.fee.find(s) != gstate.fee.end(), "no such token");
    
    gstate.fee[s] = get_fee_info(s, maker_fee, taker_fee);
    global.set(gstate, _self);

    dropsmallorders(s);
}

void dexchange::dropbytoken( const symbol& s) {
    require_auth(_self);
    cancel_orders_by_token(s, CLOSED_BY_ADMIN);
}

void dexchange::dropbypair( const symbol& a, const symbol& b) {
    require_auth(_self);
    cancel_orders_by_token_pair(a, b, CLOSED_BY_ADMIN);
}

void dexchange::erase_by_pair_orders(const std::map<uint64_t, std::set<Order>>& orders_by_pairs, const uint16_t reason) {

    for(auto by_pairs_itr = orders_by_pairs.begin(); by_pairs_itr != orders_by_pairs.end(); by_pairs_itr++) {
            
        auto pair_itr = all_orders.find(by_pairs_itr->first);
        std::list<Order> sell_orders = pair_itr->sell_orders;
        std::list<Order> buy_orders = pair_itr->buy_orders;
        std::list<Order>* orders;

        for(auto orders_itr = by_pairs_itr->second.begin(); orders_itr != by_pairs_itr->second.end(); orders_itr++) {
            if(orders_itr->sell.symbol == pair_itr->sell)
                orders = &sell_orders;
            else
                orders = &buy_orders;

            for(auto itr_to_delete = orders->begin(); itr_to_delete != orders->end(); itr_to_delete++)
                if(itr_to_delete->total_id == orders_itr->total_id) {
                    order_to_history(*itr_to_delete, reason);
                    orders->erase(itr_to_delete);
                    break;
                }
        }

        all_orders.modify(pair_itr, _self, [&] (auto& table){
            table.sell_orders = sell_orders;
            table.buy_orders = buy_orders;
        });
    }
}

void dexchange::addblacklist(const name& account) {
    require_auth(_self);
    check(blacklist.find(account.value) == blacklist.end(), "account already blacklisted");

    auto account_itr = accounts.find(account.value);
    if(account_itr != accounts.end()) {

        std::map<uint64_t, std::set<Order>> orders_by_pairs;
        auto owner_index = all_orders_info.get_index<"byowner"_n>();
        auto order_itr = owner_index.lower_bound(account_itr->key);

        while(order_itr->owner == account) {
            orders_by_pairs[order_itr->sell.symbol.raw()^order_itr->buy.symbol.raw()].insert(*order_itr);
            order_itr++;
        }

        erase_by_pair_orders(orders_by_pairs, CLOSED_ACCOUNT_BLACKLISTED);

        for(auto balance_itr = account_itr->balances.begin(); balance_itr != account_itr->balances.end(); balance_itr++) {
            asset quantity = balance_itr->second.available + balance_itr->second.used;
            if(quantity.amount != 0)
                send_transfer(account_itr->owner, quantity, std::string("This account has been blacklisted"));
        }
        accounts.erase(account_itr);
    }

    blacklist.emplace(_self, [&] (auto& acnt) {
        acnt.account = account;
        acnt.block_time = current_time_point();
    });
}

void dexchange::delblacklist(const name& account) {
    require_auth(_self);

    auto blacklist_itr = blacklist.find(account.value);
    check(blacklist_itr != blacklist.end(), "account is not blacklisted");

    blacklist.erase(blacklist_itr);
}

void dexchange::transfer(   const name&    from,
                            const name&    to,
                            const asset&   quantity,
                            const std::string&  memo )
{
    require_auth(from);

    if(from == _self || to != _self)
                return;

    check(blacklist.find(from.value) == blacklist.end(), "This account has been blacklisted");

    struct token_info balance;
    balance.available = quantity;
    balance.used = asset(0,quantity.symbol);

    auto itr = accounts.find(from.value);
    if(itr == accounts.end())
    {
        itr = accounts.emplace(_self, [&] (auto& acnt) {
            acnt.owner = from;
            acnt.key = from.value;
            acnt.balances[quantity.symbol] = balance;
            for(auto pair: gstate.permitted_pairs)
                acnt.pairs_keys[std::pair(pair.sell, pair.buy)] = pair.key^acnt.key;
        });
    }
    else
    {
        accounts.modify(itr, _self, [&] (auto& acnt){
            auto itr_balance = acnt.balances.find(quantity.symbol);
            if(itr_balance == acnt.balances.end())
                acnt.balances[quantity.symbol] = balance;
            else
                acnt.balances[quantity.symbol].available += quantity;
        });
    }
}

void dexchange::withdraw( const name& owner, const symbol& token) { 

    require_auth(owner);
    check(blacklist.find(owner.value) == blacklist.end(), "This account has been blacklisted");

    auto itr = accounts.find(owner.value);
    check(itr != accounts.end(), "no such account");

    auto balance = itr->balances.find(token);
    check(balance != itr->balances.end(), "no such token balance");
    check(balance->second.available.amount != 0, "zero token balance");

    send_transfer(owner, balance->second.available, std::string("Token/tokens have been withdrawn"));

    accounts.modify(itr, _self, [&] (auto& acnt){
        acnt.balances[token].available.amount = 0;
    });
}

#undef EOSIO_DISPATCH

#define EOSIO_DISPATCH( TYPE, MEMBERS ) \
extern "C" { \
    void apply( uint64_t receiver, uint64_t code, uint64_t action ) { \
        auto self = receiver; \
        if( action == "onerror"_n.value ) { \
            check(code == ("eosio"_n).value, "onerror action's are only valid from the \"sig\" system account"); \
        } \
        if( code == self ) { \
            if (action != ("transfer"_n).value) {\
                switch( action ) { \
                    EOSIO_DISPATCH_HELPER( TYPE, MEMBERS ) \
                } \
            }\
        } \
        else if ( action == ("transfer"_n).value ) { \
            token_transfer tt = unpack_action_data<token_transfer>(); \
            if(tt.to != "sig.feebank"_n && tt.from != eosio::name(receiver)) { \
                global_state_singleton gl(eosio::name(receiver), receiver); \
                check(gl.exists(), "no global table"); \
                globalstate gstate =  gl.get(); \
                std::string error = std::string("token contract not permitted ") + eosio::name(code).to_string(); \
                check(gstate.token_contracts.find(eosio::name(code)) != gstate.token_contracts.end(), error); \
                check(gstate.token_contracts[eosio::name(code)].symbols.find(tt.quantity.symbol) != gstate.token_contracts[eosio::name(code)].symbols.end(), " token symbol not found in permitted contract"); \
                execute_action(eosio::name(receiver), eosio::name(code), &dexchange::transfer); \
            } \
        }\
    } \
}

EOSIO_DISPATCH(dexchange,   (transfer)
                            (withdraw)
                            (order)
                            (droporders)
                            (dropall)
                            (init)
                            (addtoken)
                            (deltoken)
                            (setfee)
                            (addtokenpair)
                            (deltokenpair)
                            (dropbytoken)
                            (dropbypair)
                            (addblacklist)
                            (delblacklist)
                            )

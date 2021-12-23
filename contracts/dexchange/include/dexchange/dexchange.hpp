#pragma once

//#include <set>

#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>

#include <string>
#include <cmath>

using namespace eosio;

#define MIN_FEE_AMOUNT 10
#define GL_PERCENT 10
#define SIG_PERCENT 90
#define gl_fee_account "glexchange"
#define sig_fee_account "sigexchange"

struct token_transfer 
   {
      name from;
      name to;
      asset quantity;
      std::string memo;
   };

   struct token_info {
      eosio::asset available;
      eosio::asset used;
   };

   struct [[eosio::table, eosio::contract("dexchange")]] Account {
      eosio::name    owner;
      uint64_t       key;
      std::map<symbol, token_info> balances;
      std::map<std::pair<symbol, symbol>, uint64_t>   pairs_keys;

      uint64_t primary_key()const { return owner.value; }
   };

   using account_index = multi_index<"accounts"_n, Account>;

   struct [[eosio::table, eosio::contract("dexchange")]] BlackList {
      eosio::name    account;
      time_point     block_time;

      uint64_t primary_key()const { return account.value; }
   };

   using blacklist_index = multi_index<"blacklist"_n, BlackList>;

   struct [[eosio::table, eosio::contract("dexchange")]] Order {
      uint64_t       total_id;
      eosio::name    owner;
      double         price;
      time_point     start_time;
      asset          sell;
      asset          buy;

      asset          received;
      asset          paid;

      asset          fee;
      double         average_price;

      uint64_t primary_key()const { return start_time.elapsed.count()^total_id; } // unique
      uint64_t by_id() const { return total_id; }
      uint64_t by_time() const { return start_time.elapsed.count(); }
      double   by_price() const { return price; }
      uint64_t by_owner() const { return owner.value; }
      uint64_t by_pair() const { return buy.symbol.raw()^sell.symbol.raw(); }
      uint64_t by_pair_owner() const { return buy.symbol.raw()^sell.symbol.raw()^owner.value; }

      uint64_t by_value() const { return buy.amount; }
      uint64_t sell_left_value() const { return sell.amount - paid.amount; }
      asset    sell_left() const { return sell - paid; }
      void     update_average_price(const asset& r, const asset& p, const asset& fee, bool convert);
   };

   using info_orders_index = multi_index< "ordersinfo"_n, Order,
                           indexed_by<"bypair"_n, const_mem_fun< Order, uint64_t, &Order::by_pair>>,
                           indexed_by<"byowner"_n, const_mem_fun< Order, uint64_t, &Order::by_owner>>,
                           indexed_by<"bypairowner"_n, const_mem_fun< Order, uint64_t, &Order::by_pair_owner>>,
                           indexed_by<"byid"_n, const_mem_fun< Order, uint64_t, &Order::by_id>>,
                           indexed_by<"byordersize"_n, const_mem_fun< Order, uint64_t, &Order::sell_left_value>>
                              >;

   struct [[eosio::table, eosio::contract("dexchange")]] Orders {
      symbol sell;
      symbol buy;
      std::list<Order> sell_orders;
      std::list<Order> buy_orders;
      uint64_t primary_key()const { return buy.raw()^sell.raw(); }
      void insert_order(Order& o);
   };

   using orders_index = multi_index< "orders"_n, Orders>;

   enum ORDER_CLOSED_STATUS {
      CLOSED_NORMALLY,
      CLOSED_BY_USER,
      CLOSED_BY_ADMIN,
      CLOSED_TOKEN_DELETED,
      CLOSED_TOKEN_PAIR_DELETED,
      CLOSED_ACCOUNT_BLACKLISTED,
      CLOSED_BY_MINIMUM_ORDER_SIZE
   };

   const std::vector<std::string> memos = {
   "Fill order",
   "Order/orders have been canceled by user",
   "Order/orders have been canceled by admin",
   "This token has been removed from the exchange",
   "This token pair has been removed from the exchange",
   "This account has been blacklisted",
   "The order amount does not meet the requirements of the exchange."
   };

   struct [[eosio::table, eosio::contract("dexchange")]] History {
      uint64_t       total_id;
      uint8_t        close_status = 0;
      eosio::name    owner;
      time_point     start_time;
      time_point     end_time;      
      asset          sell;
      asset          buy;
      asset          received;
      asset          paid;
      asset          fee;
      double         price;
      double         average_price;
      uint64_t primary_key()const { return start_time.elapsed.count() ^ total_id; } // unique
      uint64_t by_pair()const { return received.symbol.raw()^paid.symbol.raw(); }
      uint64_t by_owner()const { return owner.value; }
      uint64_t by_pair_owner() const { return received.symbol.raw()^paid.symbol.raw()^owner.value; }
      uint64_t by_end_time() const { return end_time.elapsed.count(); }
      uint64_t by_end_time_owner() const { return end_time.elapsed.count()^owner.value; }
   };

   using orders_history_index = multi_index< "history"_n, History, 
                                 indexed_by<"bypair"_n, const_mem_fun< History, uint64_t, &History::by_pair>>,
                                 indexed_by<"byowner"_n, const_mem_fun< History, uint64_t, &History::by_owner>>,
                                 indexed_by<"bypairowner"_n, const_mem_fun< History, uint64_t, &History::by_pair_owner>>,
                                 indexed_by<"byendtime"_n, const_mem_fun< History, uint64_t, &History::by_end_time>>,
                                 indexed_by<"byendtowner"_n, const_mem_fun< History, uint64_t, &History::by_end_time_owner>>
                                 >;

   struct [[eosio::table, eosio::contract("dexchange")]] Bucket {
      uint64_t          id;
      uint64_t          bucket;
      time_point_sec    open;
      symbol            base;
      symbol            quote;
      double            high_base;
      double            low_base;
      double            open_base;
      double            close_base;
      double            base_volume;
      double            quote_volume;

      uint64_t    primary_key()const { return id; }
      uint64_t    by_pair()const { return base.raw()^quote.raw(); }
      uint64_t    by_pair_time()const { return base.raw()^quote.raw()^open.sec_since_epoch(); }

      void update(asset& sell, asset& buy, double price);
   };

   using bucket_index1 = multi_index< "b1minute"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index2 = multi_index< "b5minutes"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index3 = multi_index< "b15minutes"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index4 = multi_index< "bhalfhour"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index5 = multi_index< "b1hour"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index6 = multi_index< "b4hours"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;
   using bucket_index7 = multi_index< "b24hours"_n, Bucket,
                           indexed_by<"bypair"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair>>, 
                           indexed_by<"bypairtime"_n, const_mem_fun< Bucket, uint64_t, &Bucket::by_pair_time>>
                           >;

   struct Pair_info {
      symbol   sell;
      symbol   buy;
      uint64_t key;

   };

   struct Symbols {
   std::set<eosio::symbol> symbols;
   };

   struct Fee_info {
      double   maker_fee;
      double   taker_fee;
      asset    min_order;
   };

   struct [[eosio::table, eosio::contract("dexchange")]] globalstate {
      uint64_t                               total_order_id = 0;
      std::map<eosio::name, Symbols>         token_contracts;
      std::map<eosio::symbol, eosio::name>   permitted_tokens;
      std::list<Pair_info>                   permitted_pairs;
      std::map<symbol, Fee_info>             fee;
      std::vector<uint32_t>                  buckets = {60, 300, 900, 1800, 3600, 14400, 86400};

      std::optional<Pair_info> pair_permitted(const asset& a, const asset& b) const;
      bool token_permitted(const asset& a) const;
   };

   using  global_state_singleton = singleton<"globalstate"_n, globalstate>;

   class [[eosio::contract("dexchange")]] dexchange : public contract {
      public:
         using contract::contract;

      dexchange( name s, name code, datastream<const char*> ds ):contract(s, code, ds),
         global(_self, _self.value),
         accounts(get_self(), get_self().value),
         blacklist(get_self(), get_self().value),
         all_orders(get_self(), get_self().value),
         all_orders_info(get_self(), get_self().value),
         orders_history(get_self(), get_self().value),
         buckets1(get_self(), get_self().value),
         buckets2(get_self(), get_self().value),
         buckets3(get_self(), get_self().value),
         buckets4(get_self(), get_self().value),
         buckets5(get_self(), get_self().value),
         buckets6(get_self(), get_self().value),
         buckets7(get_self(), get_self().value)
      {
         if(global.exists())
            gstate = global.get();
      }

      [[eosio::action]]
      void transfer( const name&    from,
                     const name&    to,
                     const asset&   quantity,
                     const std::string&  memo );

      [[eosio::action]]
      void order( const name&    owner,
                  const asset&   sell,
                  const asset&   bye);
      
      [[eosio::action]]
      void dropall( const name& owner);

      [[eosio::action]]
      void droporders( const name& owner, std::vector<uint64_t> orders);

      [[eosio::action]]
      void withdraw( const name&    owner, 
                     const symbol&  token);

      // administrating
      [[eosio::action]]
      void init();

      [[eosio::action]]
      void addtokenpair(const asset& a, const asset& b);

      [[eosio::action]]
      void deltokenpair(const asset& a, const asset& b);

      [[eosio::action]]
      void addtoken(const name& contract, const symbol& s, const double maker_fee, const double taker_fee);

      [[eosio::action]]
      void deltoken(const name& contract, const symbol& s);

      [[eosio::action]]
      void setfee(const symbol& s, const double maker_fee, const double taker_fee);

      [[eosio::action]]
      void dropbytoken(const symbol& s);

      [[eosio::action]]
      void dropbypair(const symbol& a, const symbol& b);

      [[eosio::action]]
      void addblacklist(const name& account);

      [[eosio::action]]
      void delblacklist(const name& account);

      private:
      
      global_state_singleton global;
      globalstate gstate;
      account_index accounts;
      blacklist_index   blacklist;
      orders_index  all_orders;
      info_orders_index  all_orders_info;
      orders_history_index  orders_history;
      bucket_index1   buckets1;
      bucket_index2   buckets2;
      bucket_index3   buckets3;
      bucket_index4   buckets4;
      bucket_index5   buckets5;
      bucket_index6   buckets6;
      bucket_index7   buckets7;

      uint64_t get_new_total_order_id();
      Order init_order( const name& owner, const asset& sell, const asset& buy, const symbol& sell_symbol);
      void order_to_history(const Order& o, uint8_t close_status);
      void modify_orders_info(Order& o);
      void matching(std::list<Order> sell, std::list<Order> buy);
      void update_buckets(asset& sell, asset& buy, double price);

      void drop_orders_common(std::map<uint64_t, std::set<Order>>& orders_by_pairs, std::map< name, std::map<symbol, asset>> assets_to_transfer, const uint16_t reason);
      void dropsmallorders(const symbol& s);
      void cancel_orders_by_token( const symbol& s, const uint16_t reason);
      void cancel_orders_by_token_pair( const symbol& a, const symbol& b, const uint16_t reason);
      void erase_by_pair_orders(const std::map<uint64_t, std::set<Order>>& orders_by_pairs, const uint16_t reason);
      void insert_assets_to_transfer(const Order& order, std::map< name, std::map<symbol, asset>>& assets_to_transfer);
      void erase_all_pair_orders(const Orders& orders, const uint16_t reason);

      void send_transfer(const name& to, const asset& quantity, const std::string& memo);
      void send_order_tokens(const eosio::name& from, const eosio::name& to, const eosio::asset& quantity, const eosio::asset& fee);
      void return_tokens(const eosio::symbol& s);
   };

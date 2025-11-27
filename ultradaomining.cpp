#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/system.hpp>
#include <string>

using namespace eosio;
using std::string;

class [[eosio::contract("ultradaomining")]] token : public contract
{
public:
    using contract::contract;

    [[eosio::action]]
    void create(const name &issuer, const asset &maximum_supply)
    {
        require_auth(get_self());
        check(maximum_supply.is_valid(), "invalid supply");
        check(maximum_supply.amount > 0, "max-supply must be positive");

        stats statstable(get_self(), maximum_supply.symbol.code().raw());
        auto existing = statstable.find(maximum_supply.symbol.code().raw());
        check(existing == statstable.end(), "token with symbol already exists");

        statstable.emplace(get_self(), [&](auto &s)
                           {
         s.supply = asset{0, maximum_supply.symbol};
         s.max_supply = maximum_supply;
         s.issuer = issuer;
         s.starttime = current_time_point().sec_since_epoch();
         s.minetime = s.starttime; });
    }

    [[eosio::action]]
    void issue(const name &to, const asset &quantity, const string &memo)
    {
        auto sym = quantity.symbol;
        check(sym.is_valid(), "invalid symbol name");
        check(memo.size() <= 256, "memo has more than 256 bytes");

        stats statstable(get_self(), sym.code().raw());
        const auto &st = statstable.get(sym.code().raw(), "token with symbol does not exist");
        require_auth(st.issuer);

        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must issue positive quantity");
        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
        check(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

        statstable.modify(st, same_payer, [&](auto &s)
                          { s.supply += quantity; });
        add_balance(to, quantity, st.issuer);
    }

    [[eosio::action]]
    void retire(const asset &quantity, const string &memo)
    {
        auto sym = quantity.symbol;
        check(sym.is_valid(), "invalid symbol name");
        check(memo.size() <= 256, "memo has more than 256 bytes");

        stats statstable(get_self(), sym.code().raw());
        auto &st = statstable.get(sym.code().raw(), "token with symbol does not exist");

        require_auth(st.issuer);
        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must retire positive quantity");
        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

        statstable.modify(st, same_payer, [&](auto &s)
                          { s.supply -= quantity; });
        sub_balance(st.issuer, quantity);
    }

    [[eosio::action]]
    void transfer(const name &from, const name &to, const asset &quantity, const string &memo)
    {
        check(from != to, "cannot transfer to self");
        require_auth(from);
        check(is_account(to), "to account does not exist");

        auto sym_code_raw = quantity.symbol.code().raw();
        stats statstable(get_self(), sym_code_raw);
        const auto &st = statstable.get(sym_code_raw, "token with symbol does not exist");

        if (quantity.symbol.code().to_string() == "UDAO" && !has_auth(get_self()))
        {
            check(false, "UDAO cannot be transferred by users.");
        }

        require_recipient(from);
        require_recipient(to);

        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must transfer positive quantity");
        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
        check(memo.size() <= 256, "memo has more than 256 bytes");

        auto payer = has_auth(to) ? to : from;
        sub_balance(from, quantity);
        add_balance(to, quantity, payer);
    }

    [[eosio::action]]
    void open(const name &owner, const symbol &symbol, const name &ram_payer)
    {
        require_auth(ram_payer);
        check(is_account(owner), "owner account does not exist");

        stats statstable(get_self(), symbol.code().raw());
        const auto &st = statstable.get(symbol.code().raw(), "symbol does not exist");
        check(st.supply.symbol == symbol, "symbol precision mismatch");

        accounts acnts(get_self(), owner.value);
        if (acnts.find(symbol.code().raw()) == acnts.end())
        {
            acnts.emplace(ram_payer, [&](auto &a)
                          { a.balance = asset{0, symbol}; });
        }
    }

    [[eosio::action]]
    void close(const name &owner, const symbol &symbol)
    {
        require_auth(owner);
        accounts acnts(get_self(), owner.value);
        auto it = acnts.find(symbol.code().raw());
        check(it != acnts.end(), "balance row already deleted or never existed");
        check(it->balance.amount == 0, "cannot close because the balance is not zero");
        acnts.erase(it);
    }

    [[eosio::action]]
    void setupminer(const name &user, const symbol &symbol)
    {
        require_auth(user);

        stats statstable(get_self(), symbol.code().raw());
        const auto &st = statstable.get(symbol.code().raw(), "symbol does not exist");
        check(st.supply.symbol == symbol, "symbol precision mismatch");

        accounts acnts(get_self(), user.value);
        if (acnts.find(symbol.code().raw()) == acnts.end())
        {
            acnts.emplace(user, [&](auto &a)
                          { a.balance = asset{0, symbol}; });
        }
    }

    [[eosio::on_notify("eosio.token::transfer")]]
    void claim(name from, name to, asset quantity, string memo)
    {
        if (to != get_self() || from == get_self())
            return;

        accounts acnts(get_self(), from.value);
        auto tor = acnts.find(symbol_code("UDAO").raw());
        check(tor != acnts.end(), "must initialize UDAO before mining");

        action{
            permission_level{get_self(), "active"_n},
            "eosio.token"_n,
            "transfer"_n,
            std::make_tuple(get_self(), from, quantity, string("Refund UOS"))}
            .send();

        int minetime = get_last_mine(get_self(), symbol_code("UDAO"));
        int currenttime = current_time_point().sec_since_epoch();
        int timepassed = currenttime - minetime;

        asset supply = get_supply(get_self(), symbol_code("UDAO"));
        asset reward = get_reward(supply);
        asset balance = get_balance(get_self(), get_self(), symbol_code("UDAO"));

        if (timepassed >= 600)
        {
            int rewardcount = timepassed / 600;
            asset issuereward = reward * rewardcount;

            action{
                permission_level{get_self(), "active"_n},
                get_self(),
                "issue"_n,
                std::make_tuple(get_self(), issuereward, string("Issue UDAO"))}
                .send();

            balance += issuereward;

            stats statstable(get_self(), symbol_code("UDAO").raw());
            auto &st = statstable.get(symbol_code("UDAO").raw());
            statstable.modify(st, same_payer, [&](auto &s)
                              { s.minetime = currenttime; });
        }

        balance /= 40000;

        if (balance.amount > 0)
        {
            action{
                permission_level{get_self(), "active"_n},
                get_self(),
                "transfer"_n,
                std::make_tuple(get_self(), from, balance, string("Mine UDAO"))}
                .send();
        }
    }

    static asset get_supply(const name &contract, const symbol_code &sym_code)
    {
        stats statstable(contract, sym_code.raw());
        return statstable.get(sym_code.raw()).supply;
    }

    static int get_last_mine(const name &contract, const symbol_code &sym_code)
    {
        stats statstable(contract, sym_code.raw());
        return statstable.get(sym_code.raw()).minetime;
    }

    static asset get_balance(const name &contract, const name &owner, const symbol_code &sym_code)
    {
        accounts acnts(contract, owner.value);
        return acnts.get(sym_code.raw()).balance;
    }

    asset get_reward(asset current_supply)
    {
        int supply_unit = current_supply.amount / 10000 / 10000;
        asset reward{0, symbol("UDAO", 8)};

        if (supply_unit <= 10500000)
            reward.amount = 5000000000;
        else if (supply_unit <= 15750000)
            reward.amount = 2500000000;
        else if (supply_unit <= 18375000)
            reward.amount = 1250000000;
        else if (supply_unit <= 19687500)
            reward.amount = 625000000;
        else if (supply_unit <= 20343750)
            reward.amount = 312500000;
        else if (supply_unit <= 20671875)
            reward.amount = 156250000;
        else if (supply_unit <= 20835938)
            reward.amount = 78125000;
        else if (supply_unit <= 20917969)
            reward.amount = 39062500;
        else if (supply_unit <= 20958984)
            reward.amount = 19531250;
        else if (supply_unit <= 20979492)
            reward.amount = 9765625;
        else if (supply_unit <= 20989746)
            reward.amount = 4882813;
        else if (supply_unit <= 20994873)
            reward.amount = 2441406;
        else if (supply_unit <= 20997437)
            reward.amount = 1220703;
        else if (supply_unit <= 20998718)
            reward.amount = 610352;
        else if (supply_unit <= 20999359)
            reward.amount = 305176;
        else if (supply_unit <= 20999680)
            reward.amount = 152588;
        else if (supply_unit <= 20999840)
            reward.amount = 76294;
        else if (supply_unit <= 20999920)
            reward.amount = 38147;
        else if (supply_unit <= 20999960)
            reward.amount = 19073;
        else if (supply_unit <= 20999980)
            reward.amount = 9537;
        else if (supply_unit <= 20999990)
            reward.amount = 4768;
        else if (supply_unit <= 20999995)
            reward.amount = 2384;
        else if (supply_unit <= 20999998)
            reward.amount = 1192;
        else if (supply_unit < 21000000)
            reward.amount = 596;

        return reward;
    }

private:
    struct [[eosio::table]] account
    {
        asset balance;
        uint64_t primary_key() const { return balance.symbol.code().raw(); }
    };

    struct [[eosio::table]] currency_stats
    {
        asset supply;
        asset max_supply;
        name issuer;
        int starttime;
        int minetime;
        uint64_t primary_key() const { return supply.symbol.code().raw(); }
    };

    typedef eosio::multi_index<"accounts"_n, account> accounts;
    typedef eosio::multi_index<"stat"_n, currency_stats> stats;

    void sub_balance(const name &owner, const asset &value)
    {
        accounts acnts(get_self(), owner.value);
        auto &from = acnts.get(value.symbol.code().raw(), "no balance object found");
        check(from.balance.amount >= value.amount, "overdrawn balance");
        acnts.modify(from, owner, [&](auto &a)
                     { a.balance -= value; });
    }

    void add_balance(const name &owner, const asset &value, const name &ram_payer)
    {
        accounts acnts(get_self(), owner.value);
        auto it = acnts.find(value.symbol.code().raw());
        if (it == acnts.end())
        {
            acnts.emplace(ram_payer, [&](auto &a)
                          { a.balance = value; });
        }
        else
        {
            acnts.modify(it, same_payer, [&](auto &a)
                         { a.balance += value; });
        }
    }
};

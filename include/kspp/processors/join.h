#include <memory>
#include <deque>
#include <functional>
#include <kspp/kspp.h>

#pragma once

namespace kspp {
// alternative? replace with std::shared_ptr<const kevent<K, R>> left, std::shared_ptr<const kevent<K, R>> right, std::shared_ptr<kevent<K, R>> result;  

  template<class K, class streamV, class tableV, class R>
  class left_join : public event_consumer<K, streamV>, public partition_source<K, R> {
  public:
    typedef std::function<void(const K &key, const streamV &left, const tableV &right, R &result)> value_joiner;

    left_join(topology &t, std::shared_ptr<partition_source < K, streamV>>

    stream, std::shared_ptr<materialized_source < K, tableV>> table, value_joiner f)
    : event_consumer<K, streamV>()
    , partition_source<K, R>(stream.get(), stream->partition())
    , _stream (stream)
    , _table(table)
    , _value_joiner(f) {
      _stream->add_sink([this](auto r) {
        this->_queue.push_back(r);
      });
      this->add_metric(&_lag);
    }

    ~left_join() {
      close();
    }

    std::string simple_name() const override {
      return "left_join";
    }

    void start(int64_t offset) override {
      // if we request begin - should we restart table here???
      //it seems that we should retain whatever's in the cache in as many cases as possible
      _table->start(kspp::OFFSET_STORED);
      _stream->start(offset);
    }

    void close() override {
      _table->close();
      _stream->close();
    }

    size_t queue_len() const override {
      return event_consumer < K, streamV > ::queue_len();
    }

    bool process_one(int64_t tick) override {
      if (!_table->eof()) {
        // just eat it... no join since we only joins with events????
        if (_table->process_one(tick))
          _table->commit(false);
        return true;
      }

      _stream->process_one(tick);

      if (!this->_queue.size())
        return false;

      // reuse event time & commit it from event stream
      while (this->_queue.size()) {
        auto ev = this->_queue.front();
        this->_queue.pop_front();
        _lag.add_event_time(tick, ev->event_time());
        auto table_row = _table->get(ev->record()->key());
        if (table_row) {
          if (ev->record()->value()) {
            auto new_value = std::make_shared<R>();
            _value_joiner(ev->record()->key(), *ev->record()->value(), *table_row->value(), *new_value);
            auto record = std::make_shared<krecord<K, R>>(ev->record()->key(), new_value, ev->event_time());
            this->send_to_sinks(std::make_shared<kspp::kevent<K, R>>(record, ev->id()));
          } else {
            auto record = std::make_shared<krecord<K, R>>(ev->record()->key(), nullptr, ev->event_time());
            this->send_to_sinks(std::make_shared<kspp::kevent<K, R>>(record, ev->id()));
          }
        } else {
          // join failed - table row not found
          // TBD should we send delete event here???
        }
      }
      return true;
    }

    void commit(bool flush) override {
      _table->commit(flush);
      _stream->commit(flush);
    }

    bool eof() const override {
      return _table->eof() && _stream->eof();
    }

  private:
    std::shared_ptr<partition_source < K, streamV>>   _stream;
    std::shared_ptr<materialized_source < K, tableV>> _table;
    value_joiner _value_joiner;
    metric_lag _lag;
  };
}
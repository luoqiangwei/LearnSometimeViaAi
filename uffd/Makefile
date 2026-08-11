CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
SRC     := src
BIN     := bin
DEMOS   := demo1_lazy_load demo2_gc_compact demo3_dirty_track demo4_move \
           demo5_uffd_iouring

all: $(addprefix $(BIN)/,$(DEMOS))

$(BIN)/%: $(SRC)/%.c $(SRC)/uffd_common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

# demo5 需要 liburing
$(BIN)/demo5_uffd_iouring: $(SRC)/demo5_uffd_iouring.c $(SRC)/uffd_common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -luring

$(BIN):
	mkdir -p $(BIN)

# 依次运行所有演示（uffd 需要 root 或 vm.unprivileged_userfaultfd=1）
run: all
	@for d in $(DEMOS); do \
		echo; echo "########## $$d ##########"; \
		$(BIN)/$$d || exit 1; \
	done

clean:
	rm -rf $(BIN)

# 最小冒烟测试：验证本机 uffd 事件投递是否可用（环境问题排查用）
smoke: $(BIN)/smoke_test
	$(BIN)/smoke_test

$(BIN)/smoke_test: $(SRC)/smoke_test.c $(SRC)/uffd_common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all run clean smoke

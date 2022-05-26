/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

import type {RNTesterModuleExample} from '../../types/RNTesterTypes';
import * as React from 'react';
import {Animated, View, StyleSheet} from 'react-native';
import RNTConfigurationBlock from '../../components/RNTConfigurationBlock';
import RNTesterButton from '../../components/RNTesterButton';
import ToggleNativeDriver from './utils/ToggleNativeDriver';

function AnimatedView({useNativeDriver}: {useNativeDriver: boolean}) {
  const animations = [];

  const animatedViewStyle = {
    backgroundColor: new Animated.Color('blue'),
    borderColor: new Animated.Color('orange'),
  };
  animations.push(
    Animated.timing(animatedViewStyle.backgroundColor, {
      toValue: new Animated.Color('red'),
      duration: 1000,
      useNativeDriver,
    }),
  );
  animations.push(
    Animated.timing(animatedViewStyle.borderColor, {
      toValue: new Animated.Color('purple'),
      duration: 1000,
      useNativeDriver,
    }),
  );

  const animatedTextStyle = {
    color: new Animated.Color('blue'),
  };
  animations.push(
    Animated.timing(animatedTextStyle.color, {
      toValue: new Animated.Color('red'),
      duration: 1000,
      useNativeDriver,
    }),
  );

  const animatedImageStyle = {
    tintColor: new Animated.Color('blue'),
  };
  animations.push(
    Animated.timing(animatedImageStyle.tintColor, {
      toValue: new Animated.Color('red'),
      duration: 1000,
      useNativeDriver,
    }),
  );

  const animation = Animated.parallel(animations);

  return (
    <>
      <RNTesterButton
        onPress={() => {
          animation.reset();
          animation.start();
        }}>
        Press to animate
      </RNTesterButton>
      <Animated.View style={[styles.animatedView, animatedViewStyle]} />
      <Animated.Text style={[styles.animatedText, animatedTextStyle]}>
        Hello World
      </Animated.Text>
      <Animated.Image
        style={[styles.animatedImage, animatedImageStyle]}
        source={require('../../assets/bunny.png')}
      />
    </>
  );
}

function AnimatedColorStyleExample(): React.Node {
  const [useNativeDriver, setUseNativeDriver] = React.useState(false);

  return (
    <View>
      <RNTConfigurationBlock>
        <ToggleNativeDriver
          value={useNativeDriver}
          onValueChange={setUseNativeDriver}
        />
      </RNTConfigurationBlock>
      <AnimatedView
        key={`animated-view-use-${useNativeDriver ? 'native' : 'js'}-driver`}
        useNativeDriver={useNativeDriver}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  animatedView: {
    height: 100,
    width: 100,
    borderWidth: 10,
  },
  animatedText: {
    fontSize: 20,
    fontWeight: 'bold',
  },
  animatedImage: {
    height: 100,
    width: 100,
  },
});

export default ({
  title: 'Color Styles',
  name: 'colorStyles',
  description: 'Animations of color styles.',
  render: () => <AnimatedColorStyleExample />,
}: RNTesterModuleExample);